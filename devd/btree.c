/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/align.h"
#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/limits.h"
#include "shared/lk/minmax.h"
#include "shared/lk/stddef.h"
#include "shared/compare.h"
#include "shared/dtracef.h"
#include "shared/format-dev-log.h"
#include "shared/string_wrappers.h"
#include "utask/blk.h"
#include "devd/btree.h"
#include "devd/lstore.h"

/*
 * This btree is built for devd's log store.  It uses the consistency
 * model of retrying reads and atomically performing writes with
 * reserved dirty block allocations that won't fail.  It performs
 * copy-on-write block dirtying from the root down and only writes to
 * newly allocated blocks.
 *
 * It uses a large fixed-size key.  While each tree can have item values
 * of different sizes all the items in a given tree will have the same
 * value size.  With fixed-size items we can avoid fragmentation and can
 * support partial block IO sizes.
 *
 * All of the structures in the block are 64bit aligned.  Internal item
 * structs and the tree's item values are packed together so the value
 * size must also be 64bit aligned.
 */

#define BTRF		"rt: rf %llu he %u"
#define BTRA(rt)	le64_to_cpu((rt)->ref.dev_addr), (rt)->height
#define BTBF		"bt: da %llu lv %u nr %u"
#define BTBA(bt)	le64_to_cpu((bt)->hdr.dev_addr), (bt)->level, le16_to_cpu((bt)->nr_items)
#define BTKF		"k: %llx.%llx.%llx"
#define BTKA(ky)	le64_to_cpu((ky)->k[0]), le64_to_cpu((ky)->k[1]), le64_to_cpu((ky)->k[2])

static struct rpdfs_log_btree_item *off_item(struct rpdfs_log_btree_block *bt, u16 off)
{
	BUG_ON(off < sizeof(struct rpdfs_log_btree_block));
	BUG_ON(off > (RPDFS_LOG_BTREE_BLOCK_SIZE - sizeof(struct rpdfs_log_btree_item)));

	return (void *)bt + off;
}

static u16 ind_off(struct rpdfs_log_btree_block *bt, u16 ind)
{
	return le16_to_cpu(bt->offsets[ind]);
}

/*
 * All the leaf items in the tree have the same val_size.  We record it
 * in the block header as a bit of redundancy and to make blocks
 * self-describing.  And parent blocks all have the same ref val size.
 */
static u16 block_val_size(struct rpdfs_log_btree_block *bt)
{
	return bt->level == 0 ? le16_to_cpu(bt->val_size) : sizeof(struct rpdfs_log_btree_ref);
}

/*
 * The size of the contiguous item struct and value that the offset
 * points to.
 */
static u16 item_size(struct rpdfs_log_btree_block *bt)
{
	return sizeof(struct rpdfs_log_btree_item) + block_val_size(bt);
}

/*
 * The size of the offsets array has to align the following first item
 * struct.  This also ensures that the max items is a multiple of two
 * which is helpful for the merging and splitting item counts.
 */
static u16 max_items(struct rpdfs_log_btree_block *bt)
{
	u16 sz = sizeof_field(struct rpdfs_log_btree_block, offsets[0]);
	u16 nr = RPDFS_LOG_BTREE_FREE_MAX / (sz + item_size(bt));

	return round_down(nr, RPDFS_LOG_BTREE_ALIGNMENT / sz);
}

static u16 nr_items(struct rpdfs_log_btree_block *bt)
{
	return le16_to_cpu(bt->nr_items);
}

static u16 free_off(struct rpdfs_log_btree_block *bt)
{
	u16 m = max_items(bt);

	return offsetof(struct rpdfs_log_btree_block, offsets[m]) + (nr_items(bt)) * item_size(bt);
}

static void set_ind_off(struct rpdfs_log_btree_block *bt, u16 ind, u16 off)
{
	bt->offsets[ind] = cpu_to_le16(off);
}

static struct rpdfs_log_btree_item *ind_item(struct rpdfs_log_btree_block *bt, u16 ind)
{
	return off_item(bt, ind_off(bt, ind));
}

/*
 * Return the last item as sorted by key.
 */
static struct rpdfs_log_btree_item *last_item(struct rpdfs_log_btree_block *bt)
{
	return ind_item(bt, nr_items(bt) - 1);
}

/*
 * Return the final item stored in the block.  The item with the
 * greatest offset.  It can be at any sorted position in the offsets
 * array.
 */
static struct rpdfs_log_btree_item *end_item(struct rpdfs_log_btree_block *bt)
{
	if (nr_items(bt) == 0)
		return NULL;

	return off_item(bt, free_off(bt) - item_size(bt));
}

static void *item_val(struct rpdfs_log_btree_item *item)
{
	return item + 1;
}

static struct rpdfs_log_btree_ref init_ref(struct rpdfs_log_btree_block *bt)
{
	struct rpdfs_log_btree_ref ref = {
		.dev_addr = bt ? bt->hdr.dev_addr : 0,
	};

	return ref;
}

static struct rpdfs_log_btree_ref *ind_ref(struct rpdfs_log_btree_block *bt, u16 ind)
{
	struct rpdfs_log_btree_item *item = ind_item(bt, ind);

	return item_val(item);
}

static int cmp_keys(struct rpdfs_log_btree_key *a, struct rpdfs_log_btree_key *b)
{
	return rpdfs_compare(le64_to_cpu(a->k[0]), le64_to_cpu(b->k[0])) ?:
	       rpdfs_compare(le64_to_cpu(a->k[1]), le64_to_cpu(b->k[1])) ?:
	       rpdfs_compare(le64_to_cpu(a->k[2]), le64_to_cpu(b->k[2]));
}

/*
 * Find the first index in the offsets array whose item's key is greater
 * than or equal to the search key.
 */
static u16 find_key_ind(struct rpdfs_log_btree_block *bt, struct rpdfs_log_btree_key *key, int *cmp)
{
	struct rpdfs_log_btree_item *item;
	int start = 0;
	int end = (int)nr_items(bt) - 1;
	int ind = 0;

	*cmp = 1;

	while (start <= end) {
		ind = (start + end) >> 1;
		item = ind_item(bt, ind);

		*cmp = cmp_keys(key, &item->key);
		if (*cmp < 0)
			end = ind - 1;
		else if (*cmp > 0)
			start = ++ind;
		else
			break;
	}

	return ind;
}

/*
 * The caller's should have ensured that their inserting val size
 * matches the block and we double check.
 */
static void insert_item(struct rpdfs_log_btree_block *bt, u16 ind,
			struct rpdfs_log_btree_key *key, void *val, u16 val_size)
{
	struct rpdfs_log_btree_item *item;
	u16 off;

	BUG_ON(ind > (nr_items(bt) + 1));
	BUG_ON(ind >= max_items(bt));
	BUG_ON(block_val_size(bt) != val_size);

	off = free_off(bt);
	memmove_array_tail(bt->offsets, ind, nr_items(bt), 1);
	le16_add_cpu(&bt->nr_items, 1);
	set_ind_off(bt, ind, off);

	item = off_item(bt, off);
	item->key = *key;
	if (val_size)
		memcpy(item_val(item), val, val_size);

	dtracef("devd_btree_insert_item", BTBF" "BTKF" ind %u off %u",
		BTBA(bt), BTKA(key), ind, off);
}

/*
 * We reclaim the free space created by the deletion by moving the end
 * item in the block into its place.  We spend cpu searching for the end
 * item's ind in the offsets array rather than storing each item's ind
 * and having to decrease all of them after a deletion from the array.
 */
static void delete_item(struct rpdfs_log_btree_block *bt, u16 ind)
{
	struct rpdfs_log_btree_item *item = ind_item(bt, ind);
	struct rpdfs_log_btree_item *end = end_item(bt);
	u16 off = ind_off(bt, ind);
	u16 size = item_size(bt);
	int cmp;

	dtracef("devd_btree_delete_item", BTBF" "BTKF" ind %u off %u",
		BTBA(bt), BTKA(&item->key), ind, off);

	memmove_array_tail(bt->offsets, ind + 1, nr_items(bt), -1);
	le16_add_cpu(&bt->nr_items, -1);
	bt->offsets[nr_items(bt)] = 0;

	if (item != end) {
		ind = find_key_ind(bt, &end->key, &cmp);
		dtracef("devd_btree_delete_item_swap", BTBF" "BTKF" ind %u cmp %d",
			BTBA(bt), BTKA(&end->key), ind, cmp);
		BUG_ON(cmp != 0);
		set_ind_off(bt, ind, off);
		memcpy(item, end, size);
		swap(item, end);
	}

	memset(item, 0, size);
}

typedef enum {
	BTF_ALLOC       = (1 << 0), /* allocate new blocks in empty references */
	BTF_PREPARE     = (1 << 1), /* read inputs but don't dirty */
	BTF_DIRTY	= (1 << 2), /* get dirty copies of existing referenced blocks */
	BTF_SPLIT       = (1 << 3), /* split full blocks on descent */
	BTF_MERGE       = (1 << 4), /* merge half empty blocks on descent */
} btf_t;

/*
 * This simple version moves items from one block to another.  It
 * re-uses the single item insertion and deletion functions so it's
 * trivial but spends time moving the offsets array.
 */
static void move_items(struct rpdfs_log_btree_block *dst, u16 dst_ind,
		       struct rpdfs_log_btree_block *src, u16 src_ind, u16 nr)
{
	struct rpdfs_log_btree_item *item;

	for (; nr-- > 0; dst_ind++) {
		item = ind_item(src, src_ind);
		insert_item(dst, dst_ind, &item->key, item_val(item), block_val_size(src));
		delete_item(src, src_ind);
	}
}

static void memset_skip(void *dst, size_t skip, int c, size_t size)
{
	BUG_ON(skip > size);

	dst += skip;
	size -= skip;
	if (size > 0)
		memset(dst, c, size);
}

static void memcpy_skip(void *dst, void *src, size_t skip, size_t size)
{
	BUG_ON(skip > size);

	dst += skip;
	src += skip;
	size -= skip;
	if (size > 0)
		memcpy(dst, src, size);
}

/*
 * Allocate a new dirty block.  We'll initialize the block either by
 * copying an existing block or by initializing the header.  We'll
 * always update the dev_addr of the returned block with the new dirty
 * address.
 */
static int alloc_block(int level, u16 val_size, struct rpdfs_log_btree_block *copy,
		       struct rpdfs_log_btree_block **bt_ret)
{
	const size_t hdr_size = sizeof_field(struct rpdfs_log_btree_block, hdr);
	struct rpdfs_log_btree_block *bt = NULL;
	struct blk_handle *hnd;
	u64 dev_addr;
	int ret;

	hnd = lstore_alloc_dirty(RPDFS_LOG_BLOCK_TYPE_BTREE, 0, &dev_addr);
	if (IS_ERR(hnd)) {
		ret = PTR_ERR(hnd);
		goto out;
	}

	bt = hnd->data;
	if (copy) {
		memcpy_skip(bt, copy, hdr_size, free_off(copy));
	} else {
		memset_skip(bt, hdr_size, 0, sizeof(struct rpdfs_log_btree_block));
		bt->level = level;
		bt->val_size = cpu_to_le16(val_size);
	}
	memset_skip(bt, free_off(bt), 0, RPDFS_LOG_BTREE_BLOCK_SIZE);

	ret = 0;
out:
	*bt_ret = bt;
	return ret;
}

/*
 * Allocate a new dirty root block.
 */
static int alloc_root_block(struct rpdfs_log_btree_root *root, u16 val_size,
			    struct rpdfs_log_btree_block **bt)
{
	int ret;

	ret = alloc_block(root->height, val_size, NULL, bt);
	if (ret == 0) {
		root->ref = init_ref(*bt);
		root->height++;
	}

	return ret;
}

/*
 * Read an existing block and possibly return a new allocated dirty
 * copy.
 */
static int get_block(struct rpdfs_log_btree_ref *ref, btf_t btf, struct rpdfs_log_btree_block **bt,
		     struct blk_ticket *tkt)
{
	struct rpdfs_log_btree_block *copy;
	struct blk_handle *hnd;
	int ret;

	/* read the existing block */
	hnd = lstore_read_block(le64_to_cpu(ref->dev_addr), RPDFS_LOG_BLOCK_TYPE_BTREE, 0, tkt);
	if (IS_ERR(hnd)) {
		ret = PTR_ERR(hnd);
		goto out;
	}

	/* allocate a dirty copy and update the ref */
	if ((btf & BTF_DIRTY) && !blk_can_modify(hnd)) {
		copy = hnd->data;
		ret = alloc_block(0, le16_to_cpu(copy->val_size), copy, bt);
		if (ret < 0)
			goto out;

		*ref = init_ref(*bt);
	} else {
		*bt = hnd->data;
	}

	ret = 0;
out:
	if (ret < 0)
		*bt = NULL;
	return ret;
}

static void insert_ref(struct rpdfs_log_btree_block *parent, u16 par_ind,
		       struct rpdfs_log_btree_key *key, struct rpdfs_log_btree_block *bt)
{
	struct rpdfs_log_btree_ref ref = init_ref(bt);

	insert_item(parent, par_ind, key, &ref, sizeof(ref));
}

static void update_ref_key(struct rpdfs_log_btree_block *parent, u16 par_ind,
			   struct rpdfs_log_btree_key *key)
{
	struct rpdfs_log_btree_item *item = ind_item(parent, par_ind);

	item->key = *key;
}

static bool should_split(struct rpdfs_log_btree_block *bt)
{
	return nr_items(bt) == max_items(bt);
}

static int split_block(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_block *parent,
		       u16 par_ind, struct rpdfs_log_btree_block *bt)
{
	static struct rpdfs_log_btree_key max_key = {
		.k[0] = (__force __le64)(~0ULL),
		.k[1] = (__force __le64)(~0ULL),
		.k[2] = (__force __le64)(~0ULL),
	};
	struct rpdfs_log_btree_block *sib;
	struct rpdfs_log_btree_item *item;
	int ret;

	ret = alloc_block(bt->level, le16_to_cpu(bt->val_size), NULL, &sib);
	if (ret < 0)
		goto out;

	if (parent == NULL) {
		ret = alloc_root_block(root, le16_to_cpu(bt->val_size), &parent);
		if (ret)
			goto out;

		par_ind = 0;
		insert_ref(parent, par_ind, &max_key, bt);
	}

	move_items(sib, 0, bt, 0, nr_items(bt) / 2);

	item = last_item(sib);
	insert_ref(parent, par_ind, &item->key, sib);

	ret = 1;
out:
	return ret;
}

static bool should_merge(struct rpdfs_log_btree_block *bt)
{
	return nr_items(bt) <= (max_items(bt) / 2);
}

static u16 merge_sibling_index(struct rpdfs_log_btree_block *parent, u16 ind)
{
	if (ind == 0)
		return ind + 1;
	else
		return ind - 1;
}

static int prepare_merge(btf_t btf, struct rpdfs_log_btree_block *parent, u16 par_ind,
			 struct blk_ticket *tkt)
{
	u16 sib_ind = merge_sibling_index(parent, par_ind);
	struct rpdfs_log_btree_ref *ref = ind_ref(parent, sib_ind);
	struct rpdfs_log_btree_block *unused;

	return get_block(ref, btf, &unused, tkt);
}

static int merge_block(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_block *parent,
		       u16 par_ind, struct rpdfs_log_btree_block *bt)
{
	struct rpdfs_log_btree_block *sib;
	struct rpdfs_log_btree_ref *ref;
	bool to_right;
	u16 sib_ind;
	u16 total;
	u16 move;
	int ret;

	sib_ind = merge_sibling_index(parent, par_ind);
	to_right = sib_ind < par_ind;

	ref = ind_ref(parent, sib_ind);
	ret = get_block(ref, BTF_DIRTY, &sib, NULL);
	if (ret < 0)
		goto out;

	total = nr_items(sib) + nr_items(bt);
	if (total <= max_items(bt))
		move = nr_items(sib);
	else
		move = nr_items(sib) - (total / 2);
	if (to_right)
		move_items(bt, 0, sib, nr_items(sib) - move, move);
	else
		move_items(bt, nr_items(bt), sib, 0, move);

	if (nr_items(sib) == 0) {
		if (!to_right)
			update_ref_key(parent, par_ind, &ind_item(parent, sib_ind)->key);
		delete_item(parent, sib_ind);
		if (nr_items(parent) == 1) {
			root->ref = init_ref(bt);
			root->height--;
		}
	} else {
		if (to_right)
			update_ref_key(parent, sib_ind, &last_item(sib)->key);
		else
			update_ref_key(parent, par_ind, &last_item(bt)->key);
	}

	ret = 1;
out:
	return ret;
}

/*
 * Handle splitting and merging during descent.  We make sure our block
 * doesn't overflow or doesn't fall under half items when items are
 * inserted or deleted.
 *
 * Splitting and merging can change the structures that the caller has
 * already descended through and built up state from.  It can add or
 * remove parent blocks, parent items, and change the block that we're
 * descending through.  Given the relatively high cost of moving the
 * items to begin with, it's a relatively low additional cost to simply
 * restart the descent rather than try and modify all the caller's state
 * to match tree changes.
 *
 * Returns > 0 when the tree shape has changed and the caller should
 * restart.
 */
static int try_split_merge(struct rpdfs_log_btree_root *root,
			   struct rpdfs_log_btree_block *parent, u16 par_ind,
			   struct rpdfs_log_btree_block *bt, btf_t btf, struct blk_ticket *tkt)
{
	int ret = 0;

	if (btf & BTF_SPLIT) {
		if (!(btf & BTF_PREPARE) && should_split(bt))
			ret = split_block(root, parent, par_ind, bt);

	} else if ((btf & BTF_MERGE) && parent != NULL && should_merge(bt)) {
		if (btf & BTF_PREPARE)
			ret = prepare_merge(btf, parent, par_ind, tkt);
		else
			ret = merge_block(root, parent, par_ind, bt);
	}

	return ret;
}

struct walk_result {
	struct rpdfs_log_btree_block *bt;
	u16 ind;
	int cmp;
};

static int btree_walk(struct rpdfs_log_btree_root *root, btf_t btf,
		      struct rpdfs_log_btree_key *key, u16 val_size, struct walk_result *res,
		      struct blk_ticket *tkt)
{
	struct rpdfs_log_btree_block *parent;
	struct rpdfs_log_btree_block *bt;
	struct rpdfs_log_btree_ref *ref;
	u16 par_ind;
	u16 ind;
	int level;
	int cmp;
	int ret;

	if (root->height == 0) {
		if (!(btf & BTF_ALLOC)) {
			ret = -ENOENT;
			goto out;
		}

		ret = alloc_root_block(root, val_size, &bt);
		if (ret < 0)
			goto out;

		dtracef("devd_btree_alloc", BTRF" "BTKF" "BTBF, BTRA(root), BTKA(key), BTBA(bt));

		ind = 0;
		cmp = 1;
		goto done;
	}

restart:
	parent = NULL;
	par_ind = 0;
	bt = NULL;
	ind = 0;
	cmp = -1;

	ref = &root->ref;
	for (level = root->height - 1; level >= 0; level--) {
		ret = get_block(ref, btf, &bt, tkt);
		if (ret < 0)
			goto out;

		ind = find_key_ind(bt, key, &cmp);

		dtracef("devd_btree_walk", BTRF" "BTKF" "BTBF" ind %u",
			BTRA(root), BTKA(key), BTBA(bt), ind);

		ret = try_split_merge(root, parent, par_ind, bt, btf, tkt);
		if (ret > 0)
			goto restart;
		else if (ret < 0)
			goto out;

		if (level >= 1) {
			parent = bt;
			par_ind = ind;
			ref = ind_ref(parent, par_ind);
		}
	}

done:
	res->bt = bt;
	res->ind = ind;
	res->cmp = cmp;

	ret = 0;
out:
	return ret;
}

int btree_prepare_insert(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
			 u16 val_size, struct blk_ticket *tkt)
{
	struct walk_result res;
	int ret;

	ret = btree_walk(root, BTF_PREPARE|BTF_SPLIT, key, val_size, &res, tkt);
	if (ret == -ENOENT)
		ret = 0;
	return ret;
}

int btree_prepare_delete(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
			 struct blk_ticket *tkt)
{
	struct walk_result res;

	return btree_walk(root, BTF_PREPARE|BTF_MERGE, key, 0, &res, tkt);
}

/*
 * Insert a new item into the tree.  Returns -EEXIST if an item already
 * exists with the given key.
 */
int btree_insert(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
		 void *val, u16 val_size)
{
	struct walk_result res;
	int ret;

	if (!IS_ALIGNED(val_size, RPDFS_LOG_BTREE_ALIGNMENT)) {
		ret = -EINVAL;
		goto out;
	}

	ret = btree_walk(root, BTF_DIRTY|BTF_ALLOC|BTF_SPLIT, key, val_size, &res, NULL);
	if (ret == 0) {
		if (block_val_size(res.bt) != val_size)
			ret = -EINVAL;
		else if (res.cmp == 0)
			ret = -EEXIST;
		else
			insert_item(res.bt, res.ind, key, val, val_size);
	}

out:
	if (ret < 0)
		dtracef("devd_btree_insert_err", BTRF" "BTKF" ret %d", BTRA(root), BTKA(key), ret);

	return ret;
}

/*
 * Set an item in the tree.  If the item already exists then its value
 * is updated.  The caller can provide a buffer to get a copy of the old
 * value.
 */
int btree_replace(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
		  void *val, void *old, u16 val_size)
{
	struct rpdfs_log_btree_item *item;
	struct walk_result res;
	int ret;

	if (!IS_ALIGNED(val_size, RPDFS_LOG_BTREE_ALIGNMENT)) {
		ret = -EINVAL;
		goto out;
	}

	ret = btree_walk(root, BTF_DIRTY|BTF_ALLOC|BTF_SPLIT, key, val_size, &res, NULL);
	if (ret == 0) {
		if (block_val_size(res.bt) != val_size) {
			ret = -EINVAL;
		} else if (res.cmp == 0) {
			item = ind_item(res.bt, res.ind);
			if (old)
				memcpy(old, item_val(item), val_size);
			memcpy(item_val(item), val, val_size);
			ret = 0;
		} else {
			insert_item(res.bt, res.ind, key, val, val_size);
		}
	}

out:
	if (ret < 0)
		dtracef("devd_btree_insert_err", BTRF" "BTKF" ret %d", BTRA(root), BTKA(key), ret);

	return ret;
}

int btree_delete(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key)
{
	struct walk_result res;
	int ret;

	ret = btree_walk(root, BTF_DIRTY|BTF_MERGE, key, 0, &res, NULL);
	if (ret == 0) {
		if (res.cmp == 0)  {
			delete_item(res.bt, res.ind);
			if (nr_items(res.bt) == 0)
				memset(root, 0, sizeof(struct rpdfs_log_btree_root));
		} else {
			ret = -ENOENT;
		}
	}

	if (ret < 0)
		dtracef("devd_btree_delete_err", BTRF" "BTKF" ret %d", BTRA(root), BTKA(key), ret);

	return ret;
}

int btree_lookup(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
		 void *val, u16 val_size, struct blk_ticket *tkt)
{
	struct rpdfs_log_btree_item *item;
	struct walk_result res;
	int ret;

	ret = btree_walk(root, 0, key, 0, &res, tkt);
	if (ret == 0) {
		if (block_val_size(res.bt) != val_size) {
			ret = -EINVAL;
		} else if (res.cmp == 0)  {
			item = ind_item(res.bt, res.ind);
			memcpy(val, item_val(item), val_size);
			ret = 0;
			dtracef("devd_btree_lookup", BTRF" "BTKF" "BTBF" ind %u",
				BTRA(root), BTKA(key), BTBA(res.bt), res.ind);
		} else {
			ret = -ENOENT;
		}
	}

	if (ret < 0)
		dtracef("devd_btree_lookup_err", BTRF" "BTKF" ret %d", BTRA(root), BTKA(key), ret);

	return ret;
}
