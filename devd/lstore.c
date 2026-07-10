/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* O_DIRECT */

#include <string.h>
#include <fcntl.h>
#include "shared/lk/crc64.h"
#include "shared/lk/err.h"
#include "shared/lk/list.h"
#include "shared/lk/stddef.h"
#include "shared/devfd.h"
#include "shared/details.h"
#include "shared/dtracef.h"
#include "shared/format-block.h"
#include "shared/format-dev-log.h"
#include "shared/log.h"
#include "shared/string_wrappers.h"
#include "utask/blk.h"
#include "utask/utask.h"
#include "utask/waiters.h"
#include "devd/btree.h"
#include "devd/lstore.h"

/*
 * This stores block data and metadata in a log-based structure in the
 * device.  Blocks on the device are referenced by their byte offset.
 * New blocks are allocated from an contiguous advancing write offset.
 * Cow btrees are used to index metadata.
 *
 * Garbage collection isn't yet implemented.  The device will be divided
 * into large segments and gc tasks will sweep their contents and
 * rewrite live data in the current segment.
 *
 * Consistent multi-block reads and writes of the structure are
 * performed in utasks.  Readers sleep while populating the block cache
 * and writers initially act as readers by first populating all the
 * inputs to their write in the block cache.  The writing tasks don't
 * sleep while dirtying blocks in memory so they're effectively atomic.
 * Each time a writer dirties blocks they increase a sequence number.
 * Readers, and preparing writers, retry when the see the sequence
 * number change while they were sleeping waiting for IO.  This avoids
 * coarse structural contention at the cpu cost of retrying operations
 * when the structure changes.
 *
 * Dirty blocks are grouped into atomic commits.  Commits write the
 * vectored contents of the dirty blocks to a single large region of the
 * device.  A single dirty commit is built at a time but many previous
 * commits can be written concurrently.  A commit's results are only
 * visible to the network once all previous commits have succeeded.  We
 * avoid worrying about unwinding to the point of previous write errors
 * by declaring that write errors are fatal and trigger removal of the
 * device.
 */

static struct lstore_instance {
	u8 dev_uuid[RPDFS_LOG_UUID_SIZE];
	struct rpdfs_log_commit_block stable_cmt;
	struct rpdfs_log_commit_block current_cmt;
	struct rpdfs_log_commit_block *dirty_cmt;
	struct utask_wait_queue waitq;
	u64 next_commit_seq;
	u64 finished_commit_seq;
	u64 next_dev_addr;
	u64 dirty_seq;
	u64 dev_size;
	int dev_fd;
} global_lstore_inst;

static inline struct rpdfs_log_btree_key block_key_to_btree_key(struct rpdfs_block_key *key)
{
	struct rpdfs_log_btree_key btk = {
		.k[0] = key->k[0],
		.k[1] = key->k[1],
		.k[2] = key->k[2],
	};
	return btk;
}

static inline struct rpdfs_block_key btree_key_to_block_key(struct rpdfs_log_btree_key *btk)
{
	struct rpdfs_block_key key = {
		.k[0] = btk->k[0],
		.k[1] = btk->k[1],
		.k[2] = btk->k[2],
	};
	return key;
}

static inline struct rpdfs_block_key dev_addr_to_block_key(u64 dev_addr)
{
	struct rpdfs_block_key key = {
		.k[0] = 0,
		.k[1] = cpu_to_le64(dev_addr),
	};
	return key;
}

static inline u64 block_key_to_dev_addr(struct rpdfs_block_key *key)
{
	return le64_to_cpu(key->k[1]);
}

static inline bool is_dev_addr_key(struct rpdfs_block_key *key)
{
	return key->k[0] == 0;
}

static int finish_commit_write(struct lstore_instance *inst, struct rpdfs_log_commit_block *cmt)
{
	u64 prev = le64_to_cpu(cmt->commit_seq) - 1;
	int ret;

	ret = utask_wait_event(&inst->waitq, inst->finished_commit_seq == prev);
	if (ret == 0) {
		inst->finished_commit_seq++;
		inst->stable_cmt = *cmt;
		utask_wake_all(&inst->waitq);
	}

	return ret;
}
static int wait_for_finished_commit(struct lstore_instance *inst, u64 commit_seq)
{
	return utask_wait_event(&inst->waitq, inst->finished_commit_seq >= commit_seq);
}

/*
 * The stable commit block is used to get stable data for responses.  It
 * reflects the most recently written commit that finished in order.
 */
static struct rpdfs_log_commit_block *stable_commit_block(struct lstore_instance *inst)
{
	return &inst->stable_cmt;
}

/*
 * The "current" root is used by writers.  It has all the changes made
 * in previous commits whose writes may not have completed.
 */
static struct rpdfs_log_commit_block *current_commit_block(struct lstore_instance *inst)
{
	return inst->dirty_cmt ?: &inst->current_cmt;
}

/*
 * The dirty commit block is going to be written in the next commit write and
 * can be modified.
 */
static struct rpdfs_log_commit_block *dirty_commit_block(struct lstore_instance *inst)
{
	return inst->dirty_cmt;
}

static u64 sample_dirty_seq(struct lstore_instance *inst)
{
	return inst->dirty_seq;
}

static bool retry_dirty_seq(struct lstore_instance *inst, u64 dirty_seq)
{
	return sample_dirty_seq(inst) != dirty_seq;
}

struct dirty_reservation {
	u64 blocks;
	u64 size;
};

/* commits that cross search offsets can have a search block added, we assume all might */
#define RSV_MAX_BLOCKS (RPDFS_LOG_COMMIT_BLOCK_MAX_ENTRIES - 1)

static void rsv_reset(struct dirty_reservation *rsv)
{
	rsv->blocks = 0;
	rsv->size = 0;
}

static void rsv_blocks(struct dirty_reservation *rsv, u64 nr, u64 size)
{
	rsv->blocks += nr;
	rsv->size += (nr * size);
}

/* insert can either add the first block or grow a new parent and split existing blocks */
static void rsv_btree_insert(struct dirty_reservation *rsv, struct rpdfs_log_btree_root *root)
{
	rsv_blocks(rsv, 1 + (root->height * 2), RPDFS_LOG_BTREE_BLOCK_SIZE);
}

/* delete can dirty parent and merge all siblings without freeing */
__unused
static void rsv_btree_delete(struct dirty_reservation *rsv, struct rpdfs_log_btree_root *root)
{
	u64 blocks;

	if (root->height > 1)
		blocks = 1 + ((root->height - 1) * 2);
	else if (root->height == 1)
		blocks = 1;
	else
		blocks = 0;

	rsv_blocks(rsv, blocks, RPDFS_LOG_BTREE_BLOCK_SIZE);
}

/* modifying an existing item dirties a path to the leaf */
__unused
static void rsv_btree_modify(struct dirty_reservation *rsv, struct rpdfs_log_btree_root *root)
{
	rsv_blocks(rsv, root->height, RPDFS_LOG_BTREE_BLOCK_SIZE);
}

static void init_log_block_header(struct lstore_instance *inst, struct rpdfs_log_block_header *hdr,
				  struct rpdfs_block_key *key, u8 type)
{
	memcpy(hdr->uuid, inst->dev_uuid, RPDFS_LOG_UUID_SIZE);
	hdr->dev_addr = cpu_to_le64(block_key_to_dev_addr(key));
	hdr->crc = 0;
	memset_zero_sizeof(hdr->_pad);
	hdr->type = type;
}

static bool type_has_header(u8 type)
{
	return type != RPDFS_LOG_BLOCK_TYPE_DATA;
}

static u64 calc_crc(const void *data, unsigned int len)
{
	return crc64_nvme(0, data, len);
}

static u64 calc_header_crc(struct rpdfs_log_block_header *hdr)
{
	__le64 tmp;
	u64 crc;

	tmp = hdr->crc;
	hdr->crc = 0;
	crc = calc_crc(hdr, RPDFS_BLOCK_SIZE);
	hdr->crc = tmp;

	return crc;
}

/*
 * Allocate a dirty block in the blk cache at the next available
 * dev_addr.
 */
static struct blk_handle *alloc_dirty_block(struct lstore_instance *inst)
{
	struct rpdfs_block_key key;
	struct blk_handle *hnd;

	key = dev_addr_to_block_key(inst->next_dev_addr);
	hnd = blk_get(&key, BGF_NEW, NULL);
	if (!IS_ERR(hnd)) {
		blk_mark_dirty(hnd);
		dtracef("lstore_alloc_dirty_block", "dev_addr %llu", inst->next_dev_addr);
		inst->next_dev_addr += RPDFS_BLOCK_SIZE;

		BUG_ON(inst->next_dev_addr >= inst->dev_size); /* XXX gc segments, etc :) */
	}

	return hnd;
}

/*
 * Prepare the commit block for recording the rest of the blocks in the
 * commit.
 */
static int start_dirty_commit(struct lstore_instance *inst)
{
	struct rpdfs_log_commit_block *cmt;
	struct blk_handle *hnd;
	int ret;

	if (inst->dirty_cmt) {
		ret = 0;
		goto out;
	}

	hnd = alloc_dirty_block(inst);
	if (IS_ERR(hnd)) {
		ret = PTR_ERR(hnd);
		goto out;
	}

	cmt = hnd->data;
	memset(cmt, 0, sizeof(struct rpdfs_log_commit_block));
	*cmt = *current_commit_block(inst);
	init_log_block_header(inst, &cmt->hdr, &hnd->key, RPDFS_LOG_BLOCK_TYPE_COMMIT);
	cmt->commit_seq = cpu_to_le64(inst->next_commit_seq);
	cmt->nr_entries = 0;

	inst->next_commit_seq++;
	inst->dirty_cmt = cmt;

	dtracef("lstore_start_dirty_commit", "commit_seq %llu", le64_to_cpu(cmt->commit_seq));
	ret = 0;
out:
	return ret;
}

/*
 * If a waiting prepare_dirty's blocks might not fit in the current
 * dirty commit then we schedule writing the commit so that the
 * prepare_dirty can use the next commit.
 */
static bool reservation_fits(struct lstore_instance *inst, struct dirty_reservation *rsv)
{
	struct rpdfs_log_commit_block *cmt = inst->dirty_cmt;
	bool fits;

	if (!cmt)
		return true;

	fits = rsv->blocks <= (RPDFS_LOG_COMMIT_BLOCK_MAX_ENTRIES - le16_to_cpu(cmt->nr_entries));
	if (!fits)
		blk_schedule_write_dirty();
	return fits;
}

static int lstore_blk_read(struct rpdfs_block_key *key, struct page *data_page)
{
	struct lstore_instance *inst = &global_lstore_inst;
	loff_t off = block_key_to_dev_addr(key);
	const size_t size = RPDFS_BLOCK_SIZE;
	int ret;

	if (!is_dev_addr_key(key))
		ret = -EINVAL;
	else
		ret = utask_read(inst->dev_fd, page_address(data_page), size, off);

	dtracef("lstore_blk_read", "off %llu size %llu ret %d", (u64)off, (u64)size, ret);

	return ret;
}

/*
 * The blk cache is calling us to write all the dirty blocks.  The cache
 * maintained our dirtying in dev addr order.  We first dirtied a commit
 * block and non-blocking dirtying phases ensures that the dirty blocks
 * are always a full coherent commit.
 */
static int lstore_blk_write(struct list_head *list)
{
	struct lstore_instance *inst = &global_lstore_inst;
	struct rpdfs_log_commit_block *cmt;
	struct rpdfs_log_commit_entry *ent;
	struct rpdfs_log_block_header *hdr;
	struct iovec *iov = NULL;
	struct blk_handle *hnd;
	u64 total;
	u64 off;
	u64 crc;
	u16 nr;
	int ret;
	int i;

	/* XXX should worry about IOV_MAX */

	hnd = blk_first_dirty_handle(list);
	if (!hnd) {
		ret = -EINVAL;
		goto out;
	}

	/* the first block must he a commit block, this is a pretty weak check */
	cmt = hnd->data;
	if (cmt->hdr.type != RPDFS_LOG_BLOCK_TYPE_COMMIT) {
		ret = -EINVAL;
		goto out;
	}
	off = block_key_to_dev_addr(&hnd->key);
	nr = 1 + le16_to_cpu(cmt->nr_entries);
	iov = malloc(nr * sizeof(iov[0]));
	if (!iov) {
		ret = -ENOMEM;
		goto out;
	}

	i = 0;
	iov[i].iov_base = (void *)hnd->data;
	iov[i].iov_len = RPDFS_BLOCK_SIZE;
	total = iov[i].iov_len;
	i++;

	while ((hnd = blk_next_dirty_handle(hnd, list))) {
		ent = &cmt->entries[i - 1];

		/* update the crc for metadata blocks with inline headers */
		if (type_has_header(ent->type)) {
			hdr = hnd->data;
			crc = calc_header_crc(hdr);
			hdr->crc = cpu_to_le64(crc);
			cmt->entries[i - 1].crc = hdr->crc;
		}

		dtracef("lstore_blk_write_one", "i %d type %u crc %016llx",
			i, ent->type, le64_to_cpu(ent->crc));

		if (i >= nr) {
			ret = -EINVAL;
			goto out;
		}

		total += RPDFS_BLOCK_SIZE;
		if (iov[i - 1].iov_base + iov[i - 1].iov_len == hnd->data) {
			/* it could happen! :) */
			iov[i - 1].iov_len += RPDFS_BLOCK_SIZE;
		} else {
			iov[i].iov_base = (void *)hnd->data;
			iov[i].iov_len = RPDFS_BLOCK_SIZE;
			i++;
		}
	}

	/* commit entries and blocks in the dirty list must match */
	if (i != nr) {
		ret = -EINVAL;
		goto out;
	}

	/* finally calculate commit header after updating entries */
	cmt->hdr.crc = cpu_to_le64(calc_header_crc(&cmt->hdr));

	/* clear the dirty commit so the next can be started */
	inst->current_cmt = *cmt;
	inst->dirty_cmt = NULL;

	dtracef("lstore_blk_write_submit", "off %llu total %llu", off, total);
	ret = utask_writev(inst->dev_fd, iov, nr, off, total);
	dtracef("lstore_blk_write_complete", "off %llu total %llu ret %d", off, total, ret);
	if (ret == 0)
		ret = finish_commit_write(inst, cmt);
out:
	free(iov);
	if (ret < 0)
		dtracef("lstore_blk_write_err", "ret %d", ret);
	return ret;
}

static void prepare_read(struct lstore_instance *inst, u64 *dirty_seq,
			 struct rpdfs_log_commit_block **cmt, struct blk_ticket *tkt)
{
	*dirty_seq = sample_dirty_seq(inst);
	*cmt = stable_commit_block(inst);
	blk_open_ticket(tkt);
}

static bool retry_prepare_read(struct lstore_instance *inst, u64 dirty_seq)
{
	return retry_dirty_seq(inst, dirty_seq);
}

static void prepare_dirty(struct lstore_instance *inst, u64 *dirty_seq,
			  struct rpdfs_log_commit_block **cmt, struct blk_ticket *tkt)
{
	*dirty_seq = sample_dirty_seq(inst);
	*cmt = current_commit_block(inst);
	blk_open_ticket(tkt);
}

/*
 * Returns true if another writer could have modified blocks and the
 * caller should retry its preparation to write.  In this case the
 * caller's ret is ignored.
 *
 * Passes through the caller_ret error if preparing didn't need to be
 * retried and the error can be trusted.
 *
 * This can generate errors if it didn't need to retry and the caller
 * didn't have an error already.  False is returned and the caller's
 * error is set.
 *
 * This only opens the commit.  The caller is free to not modify the
 * commit and leave the commit state as it is.  It's as though a commit
 * was already open and the caller did nothing.   However, if the caller
 * does modify the commit then they must call finish_dirty*().
 */
static bool retry_prepare_dirty(struct lstore_instance *inst, u64 dirty_seq,
				struct dirty_reservation *rsv, struct rpdfs_log_commit_block **cmt,
				int *caller_ret)
{
	bool retry = false;
	int ret = 0;

	if (rsv->blocks > RSV_MAX_BLOCKS) {
		ret = -EINVAL;
		goto out;
	}

	if (retry_dirty_seq(inst, dirty_seq)) {
		retry = true;
		goto out;
	}

	if (*caller_ret < 0)
		goto out;

	ret = utask_wait_event(&inst->waitq, reservation_fits(inst, rsv)) ?:
	      start_dirty_commit(inst);
	if (ret < 0)
		goto out;

	/* make sure to have caller reference dirty root */
	*cmt = dirty_commit_block(inst);
	ret = 0;
out:
	if (!retry && ret < 0)
		*caller_ret = ret;
	dtracef("lstore_retry_prepare_dirty", "caller_ret %d retry %u", *caller_ret, retry);
	return retry;
}

/*
 * The caller has finished dirtying blocks.  We update the dirty_seq so
 * that readers will retry with the new dirtied blocks.
 *
 * The caller has finished with the input blocks that were protected by
 * the ticket.
 *
 * We trigger writing of the current dirty commit and wait for it to
 * finish.  It will run after all runnable tasks so we'll batch as much
 * in the current dirty commit as is ready before we'd wait for more
 * events.
 */
static int finish_dirty_sync(struct lstore_instance *inst, struct blk_ticket *tkt)
{
	struct rpdfs_log_commit_block *cmt = inst->dirty_cmt;
	u64 commit_seq = le64_to_cpu(cmt->commit_seq);

	inst->dirty_seq++;

	blk_close_ticket(tkt);
	blk_schedule_write_dirty();

	return wait_for_finished_commit(inst, commit_seq);
}

/*
 * As we write commits we make sure that blocks at regular offsets are
 * metadata blocks and can be used to find commits.  They're either
 * commit blocks or search blocks which only exist to reference their
 * commit block.
 */
#define SEARCH_OFFSET	   (32 * 1024 * 1024)
#define SEARCH_OFFSET_SHIFT const_ilog2(SEARCH_OFFSET)
static struct blk_handle *inject_search(struct lstore_instance *inst)
{
	struct rpdfs_log_search_block *search;
	struct rpdfs_log_commit_block *cmt;
	struct blk_handle *hnd;

	hnd = lstore_alloc_dirty(RPDFS_LOG_BLOCK_TYPE_SEARCH, 0, NULL);
	if (IS_ERR(hnd))
		goto out;

	cmt = dirty_commit_block(inst);
	search = hnd->data;
	init_log_block_header(inst, &search->hdr, &hnd->key, RPDFS_LOG_BLOCK_TYPE_SEARCH);
	search->commit_dev_addr = cmt->hdr.dev_addr;

out:
	return hnd;
}

/*
 * Allocate the next dirty block and record it in the current dirty
 * commit.  This initializes the block header and the caller is
 * responsible for initializing the rest of the block contents.
 *
 * We have a scary almost-recursive hack here to inject search blocks.
 * We peek ahead at the next offset to see if we need to inject a search
 * block.
 */
struct blk_handle *lstore_alloc_dirty(u8 type, __le64 crc, u64 *dev_addr)
{
	struct lstore_instance *inst = &global_lstore_inst;
	struct rpdfs_log_commit_block *cmt = inst->dirty_cmt;
	struct rpdfs_log_commit_entry *ent;
	struct rpdfs_log_block_header *hdr;
	struct blk_handle *hnd;

	/* storing commit blocks in commit entries doesn't make a ton of sense.. */
	if (type == RPDFS_LOG_BLOCK_TYPE_COMMIT) {
		hnd = ERR_PTR(-EINVAL);
		goto out;
	}

	/* peek ahead and alloc search */
	if ((inst->next_dev_addr % SEARCH_OFFSET) == 0 && type != RPDFS_LOG_BLOCK_TYPE_SEARCH) {
		hnd = inject_search(inst);
		if (IS_ERR(hnd))
			goto out;
	}

	hnd = alloc_dirty_block(inst);
	if (IS_ERR(hnd))
		goto out;

	if (type_has_header(type)) {
		hdr = hnd->data;
		init_log_block_header(inst, hdr, &hnd->key, type);
		hdr->crc = crc;
	}

	cmt = inst->dirty_cmt;
	ent = &cmt->entries[le16_to_cpu(cmt->nr_entries)];
	le16_add_cpu(&cmt->nr_entries, 1);

	*ent = (struct rpdfs_log_commit_entry) {
		.crc = crc,
		.type = type,
	};

	dtracef("lstore_alloc_dirty", "commit_seq %llu ent %u",
		le64_to_cpu(cmt->commit_seq), le16_to_cpu(cmt->nr_entries) - 1);

	if (dev_addr)
		*dev_addr = block_key_to_dev_addr(&hnd->key);
out:
	return hnd;
}

/*
 * Read and cache the block at the given dev_addr.  We check its crc
 * either from its inline header or from the caller's crc.
 */
struct blk_handle *lstore_read_block(u64 dev_addr, u8 type, __le64 crc, struct blk_ticket *tkt)
{
	struct lstore_instance *inst = &global_lstore_inst;
	struct rpdfs_log_block_header *hdr;
	struct rpdfs_block_key key;
	struct blk_handle *hnd;
	bool bad_uuid;
	u64 calc = 0;

	key = dev_addr_to_block_key(dev_addr);
	hnd = blk_get(&key, 0, tkt);
	if (!IS_ERR(hnd) && !hnd->verified) {
		if (type_has_header(type)) {
			hdr = hnd->data;
			crc = hdr->crc;
			calc = calc_header_crc(hdr);
			bad_uuid = memcmp(inst->dev_uuid, hdr->uuid, RPDFS_LOG_UUID_SIZE) != 0;
		} else {
			calc = calc_crc(hnd->data, RPDFS_BLOCK_SIZE);
			bad_uuid = false;
		}
		if (calc != le64_to_cpu(crc) || bad_uuid)
			hnd = ERR_PTR(-EIO);
		else
			hnd->verified = 1;
	}

	dtracef("lstore_read_block", "dev_addr %llu type %u crc %016llx calc %016llx ret %d",
		dev_addr, type, le64_to_cpu(crc), calc, IS_ERR(hnd) ? (int)PTR_ERR(hnd) : 0);

	return hnd;
}

/*
 * We return the handle as a convenient container for the data page.  I
 * don't think we need the page abstraction for much longer.  We'll
 * probably just fill in a caller's iov with the pointer/len of the
 * contents.
 *
 * It's awkward but OK that caller's block reference escapes the
 * protection of the ticket.  The block won't be freed until something
 * calls into the blk cache and the caller copies the contents into
 * socket send buffers before sleeping.  We'll need a better story when
 * we do zero copy sends.
 */
struct blk_handle *lstore_read(struct rpdfs_block_key *key, struct rpdfs_msg_block_details *det)
{
	struct lstore_instance *inst = &global_lstore_inst;
	struct rpdfs_log_block_details ldet;
	struct rpdfs_log_commit_block *cmt;
	struct rpdfs_log_btree_key btk;
	DECLARE_BLK_TICKET(tkt);
	struct blk_handle *hnd;
	u64 dirty_seq;
	int ret;

	if (is_dev_addr_key(key)) {
		ret = -EINVAL;
		goto out;
	}

	do {
		prepare_read(inst, &dirty_seq, &cmt, &tkt);

		hnd = blk_get(key, BGF_NOALLOC, &tkt);
		if (PTR_ERR(hnd) == -ENOENT) {
			btk = block_key_to_btree_key(key);
			ret = btree_lookup(&cmt->details_root, &btk, &ldet, sizeof(ldet), &tkt);
			if (ret == 0)
				hnd = lstore_read_block(le64_to_cpu(ldet.dev_addr),
							RPDFS_LOG_BLOCK_TYPE_DATA,
							ldet.det.crc, &tkt);
			else
				hnd = ERR_PTR(ret);
		}

	} while (retry_prepare_read(inst, dirty_seq));

	if (IS_ERR(hnd)) {
		ret = PTR_ERR(hnd);
		goto out;
	}

	/* move cached contents from phys dev_addr to logical block key on use to avoid btree */
	if (is_dev_addr_key(&hnd->key)) {
		/* can trust allocated size as long as this is the only user of ->private */
		if (!hnd->private) {
			hnd->private = malloc(sizeof(struct rpdfs_msg_block_details));
			if (!hnd->private) {
				ret = -ENOMEM;
				goto out;
			}
		}
		memcpy(hnd->private, &ldet.det, sizeof(struct rpdfs_msg_block_details));
		blk_change_key(hnd, key);
	}

	memcpy(det, hnd->private, sizeof(struct rpdfs_msg_block_details));
	ret = 0;
out:
	dtracef("lstore_read", "ret %d", ret);
	if (ret < 0)
		hnd = ERR_PTR(ret);
	return hnd;
}

static void update_totals(struct rpdfs_log_commit_block *cmt, struct rpdfs_block_key *key,
			  struct rpdfs_log_block_details *new_ldet,
			  struct rpdfs_log_block_details *old_ldet)
{
	s64 change = (s64)!(new_ldet->dev_addr == 0) - (s64)!(old_ldet->dev_addr == 0);

	if (change != 0) {
		le64_add_cpu(&cmt->total_allocated, change);
		if (rpdfs_block_key_type(key) == RPDFS_BLOCK_KEY_TYPE_INODE)
			le64_add_cpu(&cmt->total_inodes, change);
	}
}

int lstore_write(struct rpdfs_block_key *key, struct page *data_page, size_t size,
		 struct rpdfs_msg_block_details *det)
{
	struct lstore_instance *inst = &global_lstore_inst;
	struct rpdfs_log_block_details old_ldet;
	struct rpdfs_log_block_details ldet;
	struct rpdfs_log_commit_block *cmt;
	struct rpdfs_log_btree_key btk;
	struct dirty_reservation rsv;
	DECLARE_BLK_TICKET(tkt);
	struct blk_handle *hnd;
	u64 dirty_seq;
	u64 dev_addr;
	u64 crc;
	int ret;

	if (is_dev_addr_key(key)) {
		ret = -EINVAL;
		goto out;
	}

	do {
		prepare_dirty(inst, &dirty_seq, &cmt, &tkt);

		rsv_reset(&rsv);
		rsv_btree_insert(&rsv, &cmt->details_root);
		rsv_blocks(&rsv, 1, size);

		btk = block_key_to_btree_key(key);
		ret = btree_prepare_insert(&cmt->details_root, &btk, sizeof(ldet), &tkt);

	} while (retry_prepare_dirty(inst, dirty_seq, &rsv, &cmt, &ret));

	if (ret == 0) {
		/* verify late to maybe see corruption in memory */
		crc = calc_crc(page_address(data_page), RPDFS_BLOCK_SIZE);
		if (crc != le64_to_cpu(det->crc)) {
			log_err("crc failure, calc %016llx rx %016llx", crc, le64_to_cpu(det->crc));
			ret = -EIO;
		}
	}

	if (ret < 0)
		goto out;

	hnd = lstore_alloc_dirty(RPDFS_LOG_BLOCK_TYPE_DATA, det->crc, &dev_addr);
	blk_set_data_page(hnd, data_page);

	memset_zero_sizeof(old_ldet);
	ldet.dev_addr = cpu_to_le64(dev_addr);
	ldet.det = *det;
	btree_replace(&cmt->details_root, &btk, &ldet, &old_ldet, sizeof(ldet));

	update_totals(cmt, key, &ldet, &old_ldet);

	ret = finish_dirty_sync(inst, &tkt);
out:
	dtracef("lstore_write", "ret %d", ret);
	return ret;
}

/*
 * We pull the current block totals directly from the stable commit.
 *
 * This is using the total blocks in the device for the total block
 * count that's sent to the count.  We'll want to account for internal
 * metadata and over-provisioning so that the total reflects blocks that
 * could be written over the wire.
 */
void lstore_get_block_counts(struct rpdfs_msg_block_counts_result *bcr)
{
	struct lstore_instance *inst = &global_lstore_inst;
	struct rpdfs_log_commit_block *cmt = stable_commit_block(inst);

	bcr->allocated = cmt->total_allocated;
	bcr->inodes = cmt->total_inodes;
	bcr->total = cpu_to_le64(inst->dev_size >> RPDFS_BLOCK_SHIFT);
}

/*
 * Find the greatest address that's a multiple of the search offset and
 * which contains a valid block.  The most recent commit should follow
 * soon after.
 *
 * This binary search hack only works while we're performing one forward
 * sweep of commits through the device.
 */
static int find_recent_commit(struct lstore_instance *inst, u64 size, u64 *recent)
{
	struct rpdfs_log_search_block *search;
	struct rpdfs_log_block_header *hdr;
	struct blk_handle *hnd;
	s64 start = 0;
	s64 last = (size - 1) >> SEARCH_OFFSET_SHIFT;
	s64 mid;
	s64 dev_addr;

	*recent = U64_MAX;

	while (start <= last) {
		mid = (start + last) >> 2;
		dev_addr = mid << SEARCH_OFFSET_SHIFT;

		hnd = lstore_read_block(dev_addr, RPDFS_LOG_BLOCK_TYPE_COMMIT, 0, NULL);
		if (IS_ERR(hnd))
			hnd = lstore_read_block(dev_addr, RPDFS_LOG_BLOCK_TYPE_SEARCH, 0, NULL);
		if (IS_ERR(hnd)) {
			last = mid - 1;
		} else {
			start = mid + 1;
			hdr = hnd->data;
			if (hdr->type == RPDFS_LOG_BLOCK_TYPE_SEARCH) {
				search = hnd->data;
				dev_addr = le64_to_cpu(search->commit_dev_addr);
			}

			if (*recent == U64_MAX || dev_addr > *recent)
				*recent = dev_addr;
		}
	}

	return *recent == U64_MAX ? -EINVAL : 0;
}

static u64 addr_after_commit(struct rpdfs_log_commit_block *cmt)
{
	return le64_to_cpu(cmt->hdr.dev_addr) +
	       ((1 + le16_to_cpu(cmt->nr_entries)) * RPDFS_BLOCK_SIZE);
}

/*
 * Verify that all of the commit's blocks were written.
 */
static int verify_commit(struct lstore_instance *inst, u64 addr, u64 *next_addr)
{
	struct rpdfs_log_commit_block *cmt;
	struct rpdfs_log_commit_entry *ent;
	struct blk_handle *hnd;
	DECLARE_BLK_TICKET(tkt);
	int ret;
	int i;

	blk_open_ticket(&tkt);

	hnd = lstore_read_block(addr, RPDFS_LOG_BLOCK_TYPE_COMMIT, 0, &tkt);
	if (IS_ERR(hnd)) {
		ret = PTR_ERR(hnd);
		goto out;
	}

	cmt = hnd->data;
	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];

		addr += RPDFS_BLOCK_SIZE;
		hnd = lstore_read_block(addr, ent->type, ent->crc, &tkt);
		if (IS_ERR(hnd)) {
			ret = PTR_ERR(hnd);
			goto out;
		}
	}

	*next_addr = addr_after_commit(cmt);
	ret = 0;
out:
	blk_close_ticket(&tkt);
	return ret;
}

/*
 * Walk a contiguous sequence of commits.  Return the address of the
 * last commit that had all its blocks written.
 */
static int last_verified_commit(struct lstore_instance *inst, u64 start, u64 *stable)
{
	u64 next_addr;
	u64 addr;
	int ret;

	*stable = U64_MAX;

	for (addr = start; addr < roundup(start + 1, SEARCH_OFFSET); ) {
		ret = verify_commit(inst, addr, &next_addr);
		if (ret < 0)
			break;

		*stable = addr;
		addr = next_addr;
	}

	return *stable == U64_MAX ? -EINVAL : 0;
}

/*
 * Find and load the last written stable commit in the device.
 */
static int load_stable_commit(struct lstore_instance *inst, u64 size)
{
	struct rpdfs_log_commit_block *cmt;
	struct rpdfs_block_key key;
	struct blk_handle *hnd;
	u64 recent;
	u64 addr;
	int ret;

	/* raw read to initialize uuid, full read verifies the rest of the block */
	key = dev_addr_to_block_key(0);
	hnd = blk_get(&key, 0, NULL);
	if (IS_ERR(hnd)) {
		ret = PTR_ERR(hnd);
		goto out;
	}
	cmt = hnd->data;
	memcpy(inst->dev_uuid, cmt->hdr.uuid, RPDFS_LOG_UUID_SIZE);

	ret = find_recent_commit(inst, size, &recent);
	if (ret < 0)
		goto out;

	ret = last_verified_commit(inst, recent, &addr);
	if (ret < 0 && recent > 0)
		ret = last_verified_commit(inst, recent - SEARCH_OFFSET, &addr);
	if (ret < 0)
		goto out;

	hnd = lstore_read_block(addr, RPDFS_LOG_BLOCK_TYPE_COMMIT, 0, NULL);
	if (IS_ERR(hnd)) {
		ret = PTR_ERR(hnd);
		log_err("failed to read initial commit block: "ENOF, ENOA(-ret));
		goto out;
	}

	cmt = hnd->data;
	inst->stable_cmt = *cmt;
	inst->current_cmt = *cmt;
	inst->finished_commit_seq = le64_to_cpu(cmt->commit_seq);
	inst->next_commit_seq = inst->finished_commit_seq + 1;
	inst->next_dev_addr = addr_after_commit(cmt);

	ret = 0;
out:
	return ret;
}

static struct blk_ops lstore_blk_ops = {
	.read = lstore_blk_read,
	.write = lstore_blk_write,
};

int lstore_init(char *dev_path)
{
	struct lstore_instance *inst = &global_lstore_inst;
	int oflags;
	int ret;

	memset(inst, 0, sizeof(struct lstore_instance));
	utask_init_wait_queue(&inst->waitq);
	inst->dev_fd = -1;

	oflags = O_RDWR | O_DIRECT;
	inst->dev_fd = open(dev_path, oflags, O_RDWR);
	if (inst->dev_fd < 0 && errno == EINVAL) {
		oflags &= ~O_DIRECT;
		errno = 0;
		inst->dev_fd = open(dev_path, oflags, O_RDWR);
		if (inst->dev_fd >= 0)
			log("O_DIRECT not supported on '%s', using buffered", dev_path);
	}
	if (inst->dev_fd < 0) {
		ret = -errno;
		log("error opening device '%s' :" ENOF, dev_path, ENOA(-ret));
		goto out;
	}

	ret = devfd_get_size(inst->dev_fd, &inst->dev_size) ?:
	      blk_init(&lstore_blk_ops) ?:
	      load_stable_commit(inst, inst->dev_size);
out:
	if (ret < 0)
		lstore_exit();
	return ret;
}

void lstore_exit(void)
{
	struct lstore_instance *inst = &global_lstore_inst;

	blk_exit();

	if (inst->dev_fd >= 0)
		close(inst->dev_fd);

	memset(inst, 0, sizeof(struct lstore_instance));
}
