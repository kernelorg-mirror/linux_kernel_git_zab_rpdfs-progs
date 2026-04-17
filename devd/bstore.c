/* SPDX-License-Identifier: GPL-2.0 */

#include <string.h>
#include <errno.h>
#include <netinet/in.h>

#include "shared/lk/bitops-le.h"
#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/crc64.h"
#include "shared/lk/err.h"
#include "shared/lk/kernel.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"
#include "shared/lk/string.h"
#include "shared/lk/overflow.h"

#include "shared/compare.h"
#include "shared/details.h"
#include "shared/dtracef.h"
#include "shared/format-block.h"
#include "shared/format-dev.h"
#include "shared/hash_table.h"
#include "shared/place.h"
#include "shared/string_wrappers.h"

#include "utask/block.h"
#include "utask/utask.h"

#include "devd/bstore.h"
#include "devd/free-map.h"

/*
 * This block store sits between devd request processing and the block
 * cache IO engine.  It manages safely reading stable blocks while new
 * dirty blocks are written, provides atomic updates to blocks and their
 * associated metadata, and indexes their metadata to accelerate
 * searches over the blocks.
 *
 * The device blocks are organized into four regions.  First are the
 * commit blocks that describe atomic changes.  Then comes the journal
 * as a staging ground for new versions of blocks to be written without
 * overwriting current blocks.  Next are two internal metadata block
 * regions used to describe stored blocks: summaries of block details
 * and the blocks full of details themselves.  Finally the bulk of the
 * device is used for storage of blocks that we send and receive over
 * the wire.
 *
 * A given block can be found at its final lba location or it can have
 * been written to the journal and not yet replayed out to its final
 * location.  We have a hash table that maps the logical block names to
 * their stable lba location.
 *
 * To write, blocks are dirtied in the block cache and are written
 * either to the journal if they were already in use or to their
 * physical lba.  Only if the writes succeed is the stable hash table
 * updated so readers will use the results of the write.
 *
 * The consistency model that governs access to the blocks between
 * readers and writers is built around the single threaded utask
 * runtime.  Readers always share references to the stable set of
 * blocks.  They never reference the dirty blocks.  Writers first act as
 * readers, getting shared references to any blocks they might need to
 * COW to write to the journal.  Their dirtying and modification of the
 * blocks is non-blocking so it is atomic and single threaded from the
 * perspective of the other tasks.  If readers sleep waiting for IO,
 * they check the lba mapping hash to see that the block hasn't changed
 * once they wake up.
 */

static struct bstore_instance {
	struct hash_table *stable_ht;
	struct hash_table *dirty_ht;
	struct hash_table *replay_ht;
	struct rpdfs_uuid dev_uuid;
	bool have_uuid;
	struct rpdfs_dev_commit_block stable_cmt;

	/* we'll want these versioned with the qlists */
	u64 nr_devds;
	u64 this_devd_pos;

	/* convenience, set from commit layout at init */
	u64 commit_blocks;
	u64 journal_lba;
	u64 summary_lba;
	u64 details_lba;
	u64 storage_lba;
	u64 storage_blocks;

	/* sum of nr_inodes in committed summaries */
	u64 total_inodes;

	struct utask *commit_tsk;
	struct utask_wait_queue commit_wq;
	struct rpdfs_dev_commit_block *dirty_cmt;
	struct cached_block *dirty_cmt_cblk;
	struct list_head cmt_dirty_list;
	u64 commit_phase;
	int last_commit_ret;

	struct utask *replay_tsk;
	struct utask_wait_queue replay_wq;
	/* static replay storage to keep it off the replay utask stack */
	struct {
		struct cached_block *cblk;
		unsigned int e;
	} replay_blocks[RPDFS_DEV_COMMIT_MAX_ENTRIES];

	/* tracking summaries that changed during a commit */
	unsigned int nr_changes;
	struct summary_change {
		u64 bnr;
		struct rpdfs_dev_summary sum;
	} changing_summaries[RPDFS_DEV_COMMIT_MAX_ENTRIES];

} global_bstore_inst = {
	.cmt_dirty_list = LIST_HEAD_INIT(global_bstore_inst.cmt_dirty_list),
};

/*
 * All the device coordinates for blocks associated with the fs bnr.
 */
struct bnr_lba_mapping {
	u64 bnr;			/* fs bnr from the wire */
	u64 lba;			/* device lba of fs bnr */
	u64 details_lba;		/* lba of details block that describes bnr block */
	u64 summary_lba;		/* lba of summary block that descubes details block */
	unsigned int details_ind;	/* entry index in details block that describes bnr */
	unsigned int summary_ind;	/* entry index in summary block that describes details */
};

/*
 * fs bnrs are round-robined across devds.  We, a devd, will only see
 * our fraction of the fs bnrs.  If we were the second (index 1) devd,
 * and there are 4 devds, we'd see bnr {1, 5, 9, ...} and we map those
 * to the contiguous lbas starting at the first storage lba on the
 * device.
 */
static int map_bnr(struct bstore_instance *inst, struct bnr_lba_mapping *map, u64 bnr)
{
	u64 off;

	off = (bnr - inst->this_devd_pos) / inst->nr_devds;
	if (off >= inst->storage_blocks)
		return -EINVAL;

	map->bnr = bnr;
	map->lba = inst->storage_lba + off;

	map->details_lba = inst->details_lba + (off / RPDFS_DEV_DETAILS_PER_BLOCK);
	map->details_ind = off % RPDFS_DEV_DETAILS_PER_BLOCK;

	off = map->details_lba - inst->details_lba;
	map->summary_lba = inst->summary_lba + (off / RPDFS_DEV_SUMMARIES_PER_BLOCK);
	map->summary_ind = off % RPDFS_DEV_SUMMARIES_PER_BLOCK;

	return 0;
}

/*
 * Return the fs bnr for a block decribed by its lba's offset from the
 * start of the storage lbas.
 */
static int map_bnr_from_storage_off(struct bstore_instance *inst, struct bnr_lba_mapping *map,
				    u64 off)
{
	return map_bnr(inst, map, (off * inst->nr_devds) + inst->this_devd_pos);
}

/*
 * This is used to get at the summary lba and ind for the details block.
 * We perform a normal mapping with the first dev_bnr recorded in the
 * details block.
 */
static void map_details_lba(struct bstore_instance *inst, struct bnr_lba_mapping *map,
			    u64 details_lba)
{
	int ret;

	ret = map_bnr_from_storage_off(inst, map, (details_lba - inst->details_lba) *
				       RPDFS_DEV_DETAILS_PER_BLOCK);
	BUG_ON(ret < 0); /* dirty commit should have valid lbas */
}

static void map_summary_lba(struct bstore_instance *inst, struct bnr_lba_mapping *map,
			    u64 summary_lba)
{
	int ret;

	ret = map_bnr_from_storage_off(inst, map, (summary_lba - inst->summary_lba) *
				       RPDFS_DEV_SUMMARIES_PER_BLOCK *
				       RPDFS_DEV_DETAILS_PER_BLOCK);
	BUG_ON(ret < 0); /* dirty commit should have valid lbas */
}

static u64 stable_commit_ctr(struct bstore_instance *inst)
{
	return le64_to_cpu(inst->stable_cmt.commit_ctr);
}

/*
 * Return the lba of the most recent stable version of a block if it's
 * found in the journal.  If the lba isn't in the hash table then the
 * real lba is returned.
 */
static u64 stable_lba(struct bstore_instance *inst, u64 lba)
{
	return htable_lookup(inst->stable_ht, lba) ?: lba;
}

/*
 * Returns a dirty lba > 0 if the lba is currently dirty and has an
 * entry in the commit.  Returns 0 if the lba hasn't yet been tracked as
 * dirty in the commit.  Can return the lba itself when the journaled
 * write is to the lba's final location.
 */
static u64 dirty_lba(struct bstore_instance *inst, u64 lba)
{
	return htable_lookup(inst->dirty_ht, lba);
}

/*
 * Returns non-zero if the lba is currently being replayed.  It has a
 * write in flight so dirty blocks should use journal blocks instead.
 */
static u64 replay_lba(struct bstore_instance *inst, u64 lba)
{
	return htable_lookup(inst->replay_ht, lba);
}

static bool lba_in_journal(struct bstore_instance *inst, u64 lba)
{
	return lba >= inst->journal_lba && lba < inst->summary_lba;
}

static bool type_has_header(u8 type)
{
	return type != RPDFS_DEV_BLOCK_TYPE_STORED;
}

static u64 calc_block_crc(void *buf, bool has_header)
{
	size_t off = has_header ? sizeof_field(struct rpdfs_dev_block_header, crc) : 0;

	return crc64_nvme(0, buf + off, RPDFS_BLOCK_SIZE - off);
}

static void init_hdr(struct bstore_instance *inst, struct rpdfs_dev_block_header *hdr, u8 type)
{
	hdr->crc = 0;
	hdr->dev_uuid = inst->dev_uuid;
	memset_zero_sizeof(hdr->pad_);
	hdr->type = type;
}

struct verify_hdr_args {
	struct bstore_instance *inst;
	u8 type;
};

static bool init_zeroed_header(struct bstore_instance *inst, struct rpdfs_dev_block_header *hdr,
			       u8 type)
{
	if (type_has_header(type) && mem_is_zero(hdr, sizeof(struct rpdfs_dev_block_header))) {
		init_hdr(inst, hdr, type);
		return true;
	}

	return false;
}

static int verify_hdr(struct cached_block *cblk, void *arg)
{
	struct rpdfs_dev_block_header *hdr = block_data_buf(cblk);
	struct verify_hdr_args *vha = arg;
	struct bstore_instance *inst = vha->inst;
	u64 crc;
	int ret;

	if (init_zeroed_header(inst, hdr, vha->type))
		return 0;

	crc = calc_block_crc(hdr, true);

	/* only verify the uuid once we've read a block and read it */
	if ((vha->type != hdr->type || crc != le64_to_cpu(hdr->crc)) ||
	    (inst->have_uuid &&
	     memcmp(&hdr->dev_uuid, &inst->dev_uuid, sizeof(struct rpdfs_uuid)))) {
		dtracef("bstore_verify_hdr_failed",
			"exp type %u crc %016llx hdr type %u crc %016llx",
			vha->type, crc, hdr->type, le64_to_cpu(hdr->crc));
		ret = -EUCLEAN;
	} else {
		ret = 0;
	}

	return ret;
}

/*
 * Read an internal metadata block and verify its header.  The block
 * cache drops the block if validation fails and remembers when
 * validation succeeds.
 *
 * Verification considers zeroed blocks as uninitialized and initializes
 * their header and returns success.
 */
static int read_block_hdr(struct bstore_instance *inst, u64 lba, u8 type,
			  struct cached_block **cblk_ret)
{
	struct verify_hdr_args vha = {
		.inst = inst,
		.type = type,
	};

	return block_read_verify(lba, verify_hdr, &vha, cblk_ret);
}

/*
 * A simple little helper that issues read-ahead in batches.
 */
static void readahead_batch(u64 x, u32 n, u64 limit, u32 stride)
{
	u32 i;

	if ((x % n) == 0) {
		for (i = 0; i < n && (x + i < limit); i++)
			block_readahead((x + i) * stride);
	}
}

/*
 * The commit phase is initialized to 0, is even when a dirty commit is
 * being built, and is odd when a commit is being written.
 */
static u64 current_commit_phase(struct bstore_instance *inst)
{
	return inst->commit_phase;
}

static bool current_commit_writing(struct bstore_instance *inst)
{
	return (inst->commit_phase & 1) == 1;
}

/*
 * Wait until the commit that the caller sampled is done.  The phase is
 * odd while a commit is being written.  Setting the low (odd) bit in
 * the caller's phase ensures that we wait for the commit they sampled
 * to complete whether it was dirtying or being written.
 */
static int wait_until_commit_done(struct bstore_instance *inst, u64 phase)
{
	return utask_wait_event(&inst->commit_wq, current_commit_phase(inst) > (phase | 1));
}

static u64 journal_blocks(struct bstore_instance *inst)
{
	return inst->summary_lba - inst->journal_lba;
}

static u64 commit_ctr_lba(struct bstore_instance *inst, u64 ctr)
{
	return ctr % inst->commit_blocks;
}

static u64 journal_index_ctr_lba(struct bstore_instance *inst, u64 ctr)
{
	return inst->journal_lba + (ctr % journal_blocks(inst));
}

#define REPLAY_START_PCT 80
#define REPLAY_STOP_PCT 50
#define REPLAY_WAIT_PCT 95

static int journal_used_pct(struct bstore_instance *inst)
{
	struct rpdfs_dev_commit_block *cmt = &inst->stable_cmt;
	int c = (le64_to_cpu(cmt->commit_ctr) - le64_to_cpu(cmt->oldest_commit_ctr) + 1) * 100 /
		inst->commit_blocks;
	int j = (le64_to_cpu(cmt->journal_head_ctr) - le64_to_cpu(cmt->journal_tail_ctr)) * 100 /
		journal_blocks(inst);

	return max(c, j);
}

static bool cmt_entries_fit(struct rpdfs_dev_commit_block *cmt, u16 additional)
{
	return !cmt || (le16_to_cpu(cmt->nr_entries) + additional) <= RPDFS_DEV_COMMIT_MAX_ENTRIES;
}

/*
 * Prepare resources for the caller to add block_count dirty blocks to
 * the dirty commit without blocking.
 *
 * If there isn't a dirty commit then we allocate a new commit block.
 *
 * If there isn't room in the commit then whoever added the current
 * blocks will wake the commit task which will write the commit once all
 * runnable tasks are done.  We wait until the commit finishes and
 * there's room again.
 *
 * If this returns success then the caller must call
 * finish_dirty_commit() which will block waiting for the commit to be
 * written.
 *
 * If we had to wait for a dirty commit, and the stable commit changed
 * in the interim, then we return -EAGAIN and the caller should retry.
 */
static int prepare_dirty_commit(struct bstore_instance *inst, u64 stable_ctr,
			        struct list_head *pool, u16 block_count,
				struct rpdfs_dev_commit_block **cmt_ret)
{
	struct rpdfs_dev_commit_block *stable = &inst->stable_cmt;
	struct rpdfs_dev_commit_block *cmt;
	u64 commit_ctr;
	u64 lba;
	int ret;

	if (WARN_ON_ONCE(block_count > RPDFS_DEV_COMMIT_MAX_ENTRIES)) {
		ret = -EINVAL;
		goto out;
	}

	/* kick off replay if the journal is getting full, waiting if it's too full */
	if (journal_used_pct(inst) > REPLAY_START_PCT)
		utask_wake_task(inst->replay_tsk);
	ret = utask_wait_event(&inst->commit_wq, journal_used_pct(inst) < REPLAY_WAIT_PCT);
	if (ret < 0)
		goto out;

	/* wait for writing commit to finish and for room for our blocks */
	ret = utask_wait_event(&inst->commit_wq, !current_commit_writing(inst) &&
			                         cmt_entries_fit(inst->dirty_cmt, block_count));
	if (ret < 0)
		goto out;

	/* verify inputs after possibly sleeping */
	if (stable_ctr != stable_commit_ctr(inst)) {
		ret = -EAGAIN;
		goto out;
	}

	ret = block_alloc_pool(pool, !inst->dirty_cmt + block_count);
	if (ret < 0)
		goto out;

	cmt = inst->dirty_cmt;
	if (!cmt) {
		commit_ctr = le64_to_cpu(stable->commit_ctr) + 1;
		lba = commit_ctr_lba(inst, commit_ctr);

		ret = block_create_dirty(lba, pool, &inst->cmt_dirty_list, NULL,
					 &inst->dirty_cmt_cblk);
		BUG_ON(ret < 0); /* pool should have prevented failure */

		cmt = block_data_buf(inst->dirty_cmt_cblk);
		inst->dirty_cmt = cmt;

		init_hdr(inst, &cmt->hdr, RPDFS_DEV_BLOCK_TYPE_COMMIT);
		cmt->layout = stable->layout;
		cmt->commit_ctr = cpu_to_le64(commit_ctr);
		cmt->oldest_commit_ctr = stable->oldest_commit_ctr;
		cmt->journal_head_ctr = stable->journal_head_ctr;
		cmt->journal_tail_ctr = stable->journal_tail_ctr;
		cmt->nr_entries = 0;
		cmt->nr_in_journal = 0;
		memset_zero_sizeof(cmt->pad_);
	}

	ret = 0;
out:
	if (ret < 0)
		cmt = NULL;
	*cmt_ret = cmt;
	return ret;
}

/*
 * The caller has sampled the stable commit_ctr and gathered their
 * inputs for dirtying blocks in a commit.  This returns true if they
 * should retry.  It returns false if they can proceed, and *ret < 0 if
 * there was an error.  If *ret == 0 then the pool and commit are
 * prepared for dirtying the commit and finish_dirty_commit() must be
 * called when they're done.
 */
static bool retry_prepare_dirty(struct bstore_instance *inst, u64 stable_ctr, int *ret,
				struct list_head *pool, u16 block_count,
				struct rpdfs_dev_commit_block **cmt_ret)
{
	if (stable_ctr != stable_commit_ctr(inst))
		return true;

	if (*ret == 0) {
		*ret = prepare_dirty_commit(inst, stable_ctr, pool, block_count, cmt_ret);
		if (*ret == -EAGAIN) {
			*ret = 0;
			return true;
		}
	}

	return false;
}

/*
 * The caller has finished updating dirty blocks in the current dirty
 * commit.  This schedules the commit write task and waits for it to
 * finish.
 *
 * The first utask that starts to wait after dirtying will put the
 * commit task at the end of the run list.  Any other dirtying tasks on
 * the run list will have a chance to add their blocks to the commit,
 * but that's it.  They'll have a chance on the next commit when they're
 * woken after waiting for the commit to finish.  We might want to tune
 * that, but more likely we'd want to add support for more commits in
 * flight.
 */
static int finish_dirty_commit(struct bstore_instance *inst, struct list_head *pool)
{
	u64 phase = current_commit_phase(inst);
	int ret;

	block_free_pool(pool);
	utask_wake_task(inst->commit_tsk);

	ret = wait_until_commit_done(inst, phase);
	if (ret == 0)
	       ret = inst->last_commit_ret;

	return ret;
}

/*
 * Give the caller a reference to a dirty block that will be written in
 * the current dirty commit.  If the lba is already dirty in the commit
 * then we return a reference to that existing dirty block.
 *
 * We prefer to write to the real lba to avoid journal replay host write
 * amplification (at the cost of unfriendly write patterns for GC,
 * causing device write amplification).  We can only do this when the
 * real lba isn't currently in use -- that is, either it's current
 * stable version is in the journal or the real lba is unused.
 *
 * For now, the dirty block contents are copied from the original (or
 * are zeroed).  This could be further optimized, but it'd make block
 * naming and buffer sharing a bit more complicated.  It'd probably be
 * worth it.
 */
static void dirty_block(struct bstore_instance *inst, struct rpdfs_dev_commit_block *cmt,
		        struct list_head *pool, u64 lba, u8 type, bool lba_unused,
			struct page *data_page, struct cached_block *copy_from_cblk,
			struct cached_block **cblk)
{
	struct rpdfs_dev_commit_entry *ent;
	u64 dlba;
	u16 nr;
	int ret;

	/* only called on real lbas that would naturally be after the journal */
	BUG_ON(lba < inst->summary_lba);
	BUG_ON(lba >= inst->storage_lba + inst->storage_blocks);

	dlba = dirty_lba(inst, lba);
	if (dlba > 0) {
		ret = block_lookup(dlba, cblk);
		/* must be dirty and pinned in the block cache */
		BUG_ON(ret < 0);
		return;
	}

	if ((lba_in_journal(inst, stable_lba(inst, lba)) || lba_unused) && !replay_lba(inst, lba)) {
		dlba = lba;
	} else {
		dlba = journal_index_ctr_lba(inst, le64_to_cpu(cmt->journal_head_ctr));
		le64_add_cpu(&cmt->journal_head_ctr, 1);
		le16_add_cpu(&cmt->nr_in_journal, 1);
	}

	/* Callers must have prepared correct block count to ensure free entries */
	nr = le16_to_cpu(cmt->nr_entries);
	BUG_ON(nr >= RPDFS_DEV_COMMIT_MAX_ENTRIES);
	cmt->nr_entries = cpu_to_le16(nr + 1);
	ent = &cmt->entries[nr];

	ent->lba = cpu_to_le64(lba);
	ent->journ_lba = cpu_to_le64(dlba);
	ent->crc = 0;
	memset_zero_sizeof(ent->pad_);
	ent->type = type;

	ret = block_create_dirty(dlba, pool, &inst->cmt_dirty_list, data_page, cblk);
	BUG_ON(ret < 0); /* prepare should have preallocated */

	htable_insert(inst->dirty_ht, lba, dlba);

	if (copy_from_cblk)
		memcpy(block_data_buf(*cblk), block_data_buf(copy_from_cblk), RPDFS_BLOCK_SIZE);
	else if (!data_page)
		memset(block_data_buf(*cblk), 0, RPDFS_BLOCK_SIZE);

	init_zeroed_header(inst, block_data_buf(*cblk), type);

	dtracef("bstore_dirty_block", "lba %llu type %u journ_lba %llu", lba, type, dlba);
}

static bool details_changed(struct rpdfs_block_details *old, struct rpdfs_block_details *new)
{
	return memcmp(old, new, sizeof(struct rpdfs_block_details)) != 0;
}

static inline bool is_allocated_inode(struct rpdfs_block_details *det)
{
	u128 place = rpdfs_place_combine_le(det->place_lo, det->place_hi);

	return !rpdfs_alloc_ctr_is_free(le64_to_cpu(det->alloc_ctr)) &&
	       (rpdfs_place_type(place) == RPDFS_PLACE_INODE);
}

/*
 * Returns true if the summary entry would change when the new details
 * replace the old.  The change in the summary is stored in delta so it
 * can be added to the existing summary.
 */
static bool summary_changed(struct rpdfs_block_details *old, struct rpdfs_block_details *new,
			    struct rpdfs_dev_summary *delta)
{
	delta->alloc_count = (u8)(!rpdfs_alloc_ctr_is_free(le64_to_cpu(new->alloc_ctr))) -
			     (u8)(!rpdfs_alloc_ctr_is_free(le64_to_cpu(old->alloc_ctr)));
	delta->nr_inodes = (u8)is_allocated_inode(new) - (u8)is_allocated_inode(old);

	return delta->alloc_count | delta->nr_inodes;
}

static void add_summary(struct rpdfs_dev_summary *summary, struct rpdfs_dev_summary *delta)
{
	summary->alloc_count += delta->alloc_count;
}

static void set_free_count(u64 bnr, u8 alloc_count)
{
	free_map_set_free(bnr, RPDFS_DEV_DETAILS_PER_BLOCK - alloc_count);
}

static int compar_summary_change(const void *A, const void *B)
{
	const struct summary_change *a = A;
	const struct summary_change *b = B;

	return rpdfs_compare(a->bnr, b->bnr);
}

/*
 * A commit can change a summary entry for each details block in the
 * commit.  If the commit succeeds we want to update our runtime
 * tracking of the summaries.  We record the changes in summaries as
 * they happen rather than sweeping all the old and new versions of
 * details blocks after the commit to rediscover what changed.  At worst
 * there can be order hundreds of changed entries so we'd rather not
 * just linearly search an array.
 */
static void register_changed_summary(struct bstore_instance *inst, u64 det_lba,
				     struct rpdfs_dev_summary *old,
				     struct rpdfs_dev_summary *delta)
{
	struct bnr_lba_mapping map;
	struct summary_change key;
	struct summary_change *chg;

	map_details_lba(inst, &map, det_lba);
	key.bnr = map.bnr;

	if (inst->nr_changes > 1)
		chg = bsearch(&key, inst->changing_summaries, inst->nr_changes,
			      sizeof(inst->changing_summaries[0]), compar_summary_change);
	else if (inst->nr_changes == 1 && inst->changing_summaries[0].bnr == key.bnr)
		chg = &inst->changing_summaries[0];
	else
		chg = NULL;

	if (chg == NULL) {
		chg = &inst->changing_summaries[inst->nr_changes++];
		chg->bnr = map.bnr;
		memset(chg, 0, sizeof(struct summary_change));
	}

	/* not all details are tracked the same, see apply_ */
	chg->sum.nr_inodes += delta->nr_inodes;
	chg->sum.alloc_count = old->alloc_count + delta->alloc_count;

	if ((chg == &inst->changing_summaries[inst->nr_changes - 1]) &&
	    (inst->nr_changes > 1) && (chg->bnr < (chg - 1)->bnr))
		qsort(inst->changing_summaries, inst->nr_changes,
		      sizeof(inst->changing_summaries[0]), compar_summary_change);
}

static void apply_changed_summaries(struct bstore_instance *inst, int err)
{
	struct summary_change *chg;
	int i;

	if (err < 0)
		goto out;

	for (i = 0; i < inst->nr_changes; i++) {
		chg = &inst->changing_summaries[i];

		set_free_count(chg->bnr, chg->sum.alloc_count);
		inst->total_inodes += (s8)chg->sum.nr_inodes;
	}
out:
	inst->nr_changes = 0;
}

/*
 * Update the stable hash table's mapping of lbas to their stable
 * location in the journal.
 */
static void update_commit_lbas(struct bstore_instance *inst, struct rpdfs_dev_commit_block *cmt)
{
	struct rpdfs_dev_commit_entry *ent;
	int i;

	/* update the htable so readers get either real lba or journaled location */
	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];

		dtracef("bstore_update_stable_lba",
			"lba %llu journ_lba %llu",
			le64_to_cpu(ent->lba), le64_to_cpu(ent->journ_lba));

		if (ent->lba == ent->journ_lba)
			htable_delete(inst->stable_ht, le64_to_cpu(ent->lba));
		else
			htable_insert(inst->stable_ht, le64_to_cpu(ent->lba),
						       le64_to_cpu(ent->journ_lba));
	}
}

/*
 * Write out the current dirty commit.
 *
 * This is woken by tasks once they've successfully dirtied blocks in
 * the current commit.  When it's woken its put on the run list after
 * all other runnable tasks so they have a chance to add to the dirty
 * commit if they don't sleep.
 *
 * Once this run it doesn't block until it has updated the commit phase
 * which stops other tasks from dirtying until the write IOs finish.
 *
 * This finalizes the dirty blocks now that they won't be dirtied again.
 * We calculate their crcs and the summaries of modified detail blocks.
 */
static void write_dirty_commit(struct bstore_instance *inst, struct rpdfs_dev_commit_block *cmt)
{
	struct rpdfs_dev_commit_entry *ent;
	struct rpdfs_dev_block_header *hdr;
	struct cached_block *cblk = NULL;
	bool has_header;
	u64 crc;
	int ret;
	int i;

	/* get most recent result from replay */
	cmt->oldest_commit_ctr = inst->stable_cmt.oldest_commit_ctr;
	cmt->journal_tail_ctr = inst->stable_cmt.journal_tail_ctr;

	dtracef("bstore_commit_write",
		"phase %llu ctr %llu old_ctr %llu head_ctr %llu tail_ctr %llu ents %u in_journ %u",
		inst->commit_phase, le64_to_cpu(cmt->commit_ctr),
		le64_to_cpu(cmt->oldest_commit_ctr), le64_to_cpu(cmt->journal_head_ctr),
		le64_to_cpu(cmt->journal_tail_ctr), le16_to_cpu(cmt->nr_entries),
		le16_to_cpu(cmt->nr_in_journal));

	/* finalize the crcs on all the blocks in the commit */
	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];

		ret = block_lookup(le64_to_cpu(ent->journ_lba), &cblk);
		BUG_ON(ret); /* must be pinned and dirty */

		has_header = type_has_header(ent->type);

		crc = calc_block_crc(block_data_buf(cblk), has_header);
		ent->crc = cpu_to_le64(crc);

		if (has_header) {
			hdr = block_data_buf(cblk);
			hdr->crc = cpu_to_le64(crc);
		}

		block_putp(&cblk);
	}

	crc = calc_block_crc(cmt, true);
	cmt->hdr.crc = cpu_to_le64(crc);

	inst->commit_phase++;

	ret = block_write_dirty(&inst->cmt_dirty_list);
	apply_changed_summaries(inst, ret);
	if (ret == 0) {
		update_commit_lbas(inst, cmt);
		inst->stable_cmt = *cmt;
	}

	htable_clear(inst->dirty_ht);
	inst->dirty_cmt = NULL;
	block_putp(&inst->dirty_cmt_cblk);

	inst->last_commit_ret = ret;
	inst->commit_phase++;
	utask_wake_all(&inst->commit_wq);
}

/*
 * Woken by finishing block dirtying or by shutdown.  When shutting down
 * there may not be a dirty commit.
 */
static void commit_write_utask(void *data)
{
	struct bstore_instance *inst = &global_bstore_inst;
	struct rpdfs_dev_commit_block *cmt;

	do {
		utask_wait_event_task((cmt = inst->dirty_cmt));
		if (cmt)
			write_dirty_commit(inst, cmt);

	} while (!utask_am_canceled());
}

#define for_each_commit_block(CMT, I, LBA, JOURN_LBA) \
	for (I = 0; \
	     (I < le16_to_cpu((CMT)->nr_entries)) && ({ \
		LBA = le64_to_cpu((CMT)->entries[I].lba); \
		JOURN_LBA = le64_to_cpu((CMT)->entries[I].journ_lba); \
		true; }); \
	     I++)

#define for_each_replay_block(INST, CMT, REPLAY_NR, I, LBA, JOURN_LBA) \
	for (I = 0; \
	     (I < REPLAY_NR) && ({ \
		LBA = le64_to_cpu((CMT)->entries[(INST)->replay_blocks[I].e].lba); \
		JOURN_LBA = le64_to_cpu((CMT)->entries[(INST)->replay_blocks[I].e].journ_lba); \
		true; }); \
	     I++)

/*
 * The oldest commit and journal blocks are reclaimed by replaying the
 * blocks that are still in use out of the journal to their final lba.
 * We only have to do this for current blocks that are still in the
 * journal as recorded by the oldest commit.
 *
 * If an otherwise current block is dirty then we wait for that commit
 * to finish.  We don't want to replay the block if the dirty version is
 * written and we don't want to drop the current stable version if the
 * dirty commit fails.
 *
 * We record blocks that have replay writes in flight in a dedicated
 * hash table.  Dirtying won't try and dirty a block that we have in
 * flight.  It'll write the new version to the journal as it would have
 * if the final lba was in use.
 *
 * We only work a commit at a time as a weak limit on the amount of
 * device resources that replay can use.  It first has to read all the
 * input blocks and then write them to their stable location.
 *
 * Today this replays by reading and writing through the devd block
 * cache.  It could use device copy commands that avoid the host bw use
 * when devices support them.  (nvme in particular has a copy command.).
 */
static void replay_oldest_commit(struct bstore_instance *inst)
{
	struct rpdfs_dev_commit_block *cmt;
	struct cached_block *cmt_cblk = NULL;
	struct cached_block *cblk = NULL;
	LIST_HEAD(dirty_list);
	LIST_HEAD(pool);
	u64 journ_lba;
	u64 phase;
	u64 lba;
	u16 replay_nr = 0;
	int i;
	int ret;

	lba = commit_ctr_lba(inst, le64_to_cpu(inst->stable_cmt.oldest_commit_ctr));
	ret = read_block_hdr(inst, lba, RPDFS_DEV_BLOCK_TYPE_COMMIT, &cmt_cblk);
	if (ret < 0)
		goto out;
	cmt = block_data_buf(cmt_cblk);

	/* record and issue read-ahead on first guess at blocks to replay */
	for_each_commit_block(cmt, i, lba, journ_lba) {
		/* don't check dirty when gathering before we block */
		if (lba_in_journal(inst, journ_lba) && stable_lba(inst, lba) == journ_lba) {
			inst->replay_blocks[replay_nr].e = i;
			replay_nr++;
			block_readahead(journ_lba);
		}
	}

	/* block waiting for reads */
	for_each_replay_block(inst, cmt, replay_nr, i, lba, journ_lba) {
		ret = block_read(journ_lba, &inst->replay_blocks[i].cblk);
		if (ret < 0)
			goto out;
	}

	/* verify blocks after waiting, can drop all blocks */
	for_each_replay_block(inst, cmt, replay_nr, i, lba, journ_lba) {
		/* drop block if journaled version is no longer current stable */
		if (stable_lba(inst, lba) != journ_lba) {
			block_invalidate(inst->replay_blocks[i].cblk);
			block_putp(&inst->replay_blocks[i].cblk);
			inst->replay_blocks[i].e = 0;
			if (i != replay_nr - 1)
				swap(inst->replay_blocks[i], inst->replay_blocks[replay_nr - 1]);
			replay_nr--;
			i--;
			continue;
		}

		/* wait for commits that dirtied our replay blocks to finish */
		if (dirty_lba(inst, lba)) {
			phase = current_commit_phase(inst);
			ret = wait_until_commit_done(inst, phase);
			if (ret < 0)
				goto out;
			i = -1;
			continue;
		}
	}

	ret = block_alloc_pool(&pool, replay_nr);
	if (ret < 0)
		goto out;

	/* create dirty copies of the input journaled blocks at their final lbas */
	for_each_replay_block(inst, cmt, replay_nr, i, lba, journ_lba) {
		/* prevent commits from dirtying our write in flight */
		htable_insert(inst->replay_ht, lba, lba);

		ret = block_create_dirty(lba, &pool, &dirty_list,
					 block_data_page(inst->replay_blocks[i].cblk), &cblk);
		BUG_ON(ret < 0); /* pool should have prevented failure */

		/* done with all blocks once they're on the dirty list */
		block_putp(&inst->replay_blocks[i].cblk);
		block_putp(&cblk);
	}

	ret = block_write_dirty(&dirty_list);
	BUG_ON(ret < 0); /* XXX :) */

	for_each_replay_block(inst, cmt, replay_nr, i, lba, journ_lba) {
		/* the lba is available for dirtying again */
		htable_delete(inst->replay_ht, lba);

		/* clear stable if it was still pointing at the block in the journal */
		if (stable_lba(inst, lba) == journ_lba)
			htable_delete(inst->stable_ht, lba);

		/* lba used from now on, drop old journaled lba */
		if (block_lookup(journ_lba, &cblk) == 0) {
			block_invalidate(cblk);
			block_putp(&cblk);
		}
	}

	/* let runtime operate on result of writes, next commit will really store */
	le64_add_cpu(&inst->stable_cmt.oldest_commit_ctr, 1);
	le64_add_cpu(&inst->stable_cmt.journal_tail_ctr, le16_to_cpu(cmt->nr_in_journal));
	utask_wake_all(&inst->commit_wq);

	ret = 0;
out:
	block_free_pool(&pool);
	block_put(cmt_cblk);
	block_put(cblk);
	for (i = 0; i < replay_nr; i++) {
		block_putp(&inst->replay_blocks[i].cblk);
		inst->replay_blocks[i].e = 0;
	}
}

/*
 * Woken by finishing block dirtying or by shutdown.  When shutting down
 * there may not be a dirty commit.
 */
static void journal_replay_utask(void *data)
{
	struct bstore_instance *inst = &global_bstore_inst;
	int ret;

	do {
		ret = utask_wait_event_task(journal_used_pct(inst) > REPLAY_STOP_PCT);
		if (ret == 0)
			replay_oldest_commit(inst);

	} while (!utask_am_canceled());
}

/*
 * Give the caller a reference to the current version of the block that
 * stores the given bnr.
 *
 * The network protocol doesn't yet make use of the block details.  We
 * do read the details block to reflect the IO cost of eventually doing
 * so.
 *
 * The caller's block reference is also escaping the protection of
 * checking the stable commit.  That's OK because today they copy the
 * block into a send buffer before blocking so the block won't be
 * modified.
 */
int bstore_read(u64 bnr, struct cached_block **cblk, struct rpdfs_block_details *det)
{
	struct bstore_instance *inst = &global_bstore_inst;
	struct cached_block *det_cblk = NULL;
	struct rpdfs_dev_details_block *dblk;
	struct bnr_lba_mapping map;
	u64 ctr;
	int ret;

	ret = map_bnr(inst, &map, bnr);
	if (ret < 0)
		goto out;

	do {
		ctr = stable_commit_ctr(inst);

		block_putp(&det_cblk);
		block_putp(cblk);

		block_readahead(stable_lba(inst, map.lba));
		ret = read_block_hdr(inst, stable_lba(inst, map.details_lba),
				     RPDFS_DEV_BLOCK_TYPE_DETAILS, &det_cblk) ?:
		      block_read(stable_lba(inst, map.lba), cblk);

	} while (stable_commit_ctr(inst) != ctr);
	if (ret < 0)
		goto out;

	/* give the caller the block's details */
	dblk = block_data_buf(det_cblk);
	*det = dblk->details[map.details_ind];

out:
	block_putp(&det_cblk);

	dtracef("bstore_read", "bnr %llu ret %d", bnr, ret);
	return ret;
}

int bstore_write(u64 bnr, struct page *data_page, struct rpdfs_block_details *in_det)
{
	struct bstore_instance *inst = &global_bstore_inst;
	struct rpdfs_dev_details_block *dblk;
	struct rpdfs_dev_summary_block *stable_sblk;
	struct rpdfs_dev_summary_block *sblk;
	struct rpdfs_dev_commit_block *cmt;
	struct rpdfs_block_details *stable_det = NULL;
	struct cached_block *det_cblk = NULL;
	struct cached_block *sum_cblk = NULL;
	struct cached_block *cblk = NULL;
	struct rpdfs_dev_summary delta;
	struct bnr_lba_mapping map;
	LIST_HEAD(pool);
	u64 nr;
	int ret;

	ret = map_bnr(inst, &map, bnr);
	if (ret < 0)
		goto out;

	do {
		nr = stable_commit_ctr(inst);

		block_putp(&det_cblk);
		block_putp(&sum_cblk);

		block_readahead(stable_lba(inst, map.summary_lba));
		ret = read_block_hdr(inst, stable_lba(inst, map.details_lba),
				     RPDFS_DEV_BLOCK_TYPE_DETAILS, &det_cblk) ?:
		      read_block_hdr(inst, stable_lba(inst, map.summary_lba),
				     RPDFS_DEV_BLOCK_TYPE_SUMMARY, &sum_cblk);

	} while (retry_prepare_dirty(inst, nr, &ret, &pool, 3, &cmt));
	if (ret < 0)
		goto out;

	/* get a reference to the stable details for the block */
	dblk = block_data_buf(det_cblk);
	stable_det = &dblk->details[map.details_ind];

	/* dirty and update the details if they changed */
	if (details_changed(stable_det, in_det)) {
		dirty_block(inst, cmt, &pool, map.details_lba, RPDFS_DEV_BLOCK_TYPE_DETAILS, false,
			    NULL, det_cblk, &cblk);
		dblk = block_data_buf(cblk);
		dblk->details[map.details_ind] = *in_det;
		block_putp(&cblk);
	}

	/* dirty and update the summary entry covering the details if it changed */
	if (summary_changed(stable_det, in_det, &delta)) {
		dirty_block(inst, cmt, &pool, map.summary_lba, RPDFS_DEV_BLOCK_TYPE_SUMMARY, false,
			    NULL, sum_cblk, &cblk);
		stable_sblk = block_data_buf(sum_cblk);
		sblk = block_data_buf(cblk);
		add_summary(&sblk->summaries[map.summary_ind], &delta);
		register_changed_summary(inst, map.details_lba,
					 &stable_sblk->summaries[map.summary_ind], &delta);
		block_putp(&cblk);
	}

	/* and dirty the stored block with a reference to the data page */
	dirty_block(inst, cmt, &pool, map.lba, RPDFS_DEV_BLOCK_TYPE_STORED,
		    rpdfs_alloc_ctr_is_free(le64_to_cpu(stable_det->alloc_ctr)),
		    data_page, NULL, &cblk);
	block_putp(&cblk);

	ret = finish_dirty_commit(inst, &pool);
out:
	block_free_pool(&pool);
	block_putp(&det_cblk);
	block_putp(&sum_cblk);

	dtracef("bstore_write", "bnr %llu ret %d", bnr, ret);
	return ret;
}

/*
 * Fill in the caller's details for free blocks in the caller's region.
 * The interface is pretty generic, but in practice the use is tuned to
 * align to single details blocks.
 */
int bstore_get_free_details(u64 bnr, unsigned long *bmap,
			    struct rpdfs_msg_free_stripe_detail *fsd, size_t size)
{
	struct bstore_instance *inst = &global_bstore_inst;
	struct rpdfs_dev_details_block *dblk;
	struct rpdfs_block_details *det;
	struct cached_block *cblk = NULL;
	struct bnr_lba_mapping map;
	int count = 0;
	u64 ctr;
	int ret;
	int i;

	ret = map_bnr(inst, &map, bnr);
	if (ret < 0)
		goto out;

	if (map.details_ind != 0 || size > RPDFS_DEV_DETAILS_PER_BLOCK) {
		ret = -EINVAL;
		goto out;
	}

	do {
		ctr = stable_commit_ctr(inst);

		ret = read_block_hdr(inst, stable_lba(inst, map.details_lba),
				     RPDFS_DEV_BLOCK_TYPE_DETAILS, &cblk);

	} while (stable_commit_ctr(inst) != ctr);
	if (ret < 0)
		goto out;

	dblk = block_data_buf(cblk);

	/* don't advertise bnr 0 */
	for (i = map.bnr == 0 ? 1 : 0; i < size; i++) {
		det = &dblk->details[map.details_ind + i];

		if (rpdfs_alloc_ctr_is_free(le64_to_cpu(det->alloc_ctr))) {
			set_bit(i, bmap);
			fsd[i].alloc_ctr = det->alloc_ctr;
			fsd[i].wcount = det->write_ctr;
			count++;
		};
	}

	ret = 0;
out:
	block_putp(&cblk);
	return ret ?: count;
}

/*
 * Return the distance between contiguous devd storage blocks in the fs
 * bnr space.
 */
u64 bstore_contig_devd_block_bnr_distance(void)
{
	struct bstore_instance *inst = &global_bstore_inst;

	return inst->nr_devds;
}

/*
 * Verify that all the blocks in a commit were successfully written.  We
 * read the commit block, readahead all the blocks in entries, and then
 * check their crcs.
 */
static int check_complete_commit(struct bstore_instance *inst, u64 commit_ctr)
{
	struct rpdfs_dev_commit_block *cmt;
	struct rpdfs_dev_commit_entry *ent;
	struct cached_block *cmt_cblk = NULL;
	struct cached_block *cblk = NULL;
	u64 journ_lba;
	u64 lba;
	u64 crc;
	int ret;
	int i;

	lba = commit_ctr_lba(inst, commit_ctr);
	ret = read_block_hdr(inst, lba, RPDFS_DEV_BLOCK_TYPE_COMMIT, &cmt_cblk);
	if (ret < 0)
		goto out;
	cmt = block_data_buf(cmt_cblk);

	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];
		journ_lba = le64_to_cpu(ent->journ_lba);

		block_readahead(journ_lba);
	}

	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];
		journ_lba = le64_to_cpu(ent->journ_lba);

		ret = block_read(journ_lba, &cblk);
		if (ret < 0)
			goto out;

		crc = calc_block_crc(block_data_buf(cblk), type_has_header(ent->type));
		block_put(cblk);

		if (le64_to_cpu(ent->crc) != crc) {
			ret = -EINVAL;
			goto out;
		}
	}

	ret = 0;
out:
	block_put(cmt_cblk);

	dtracef("bstore_check_complete_commit", "ctr %llu ret %d", commit_ctr, ret);
	return ret;
}

/*
 * Find the most recent complete commit that was written.  We first
 * perform a binary search of commit blocks to find the greatest
 * commit_ctr.  We then make sure all its blocks were written, and check
 * the previous commit if they weren't.
 *
 * The binary search is a little tricky to picture because we're probing
 * for the small contiguous range of ctrs that can be present in the
 * ring.  We implement the search range in terms of the full precision
 * counters.  Each read maps to a commit block lba, and then we can
 * clamp the search range given the ctr we find and the adjacent ctrs
 * that could be possible in the ring of blocks.
 */
static int find_stable_commit(struct bstore_instance *inst, u64 *commit_ctr_ret)
{
	struct rpdfs_dev_commit_block *cmt;
	struct cached_block *cblk = NULL;
	u64 greatest = 0;
	u64 start = 0;
	u64 end = U64_MAX;
	u64 last;
	u64 mid;
	u64 ctr;
	u64 lba;
	int ret;

	while (start <= end) {
		mid = start + ((end - start) >> 1);
		lba = commit_ctr_lba(inst, mid);

		block_putp(&cblk);
		ret = block_read(lba, &cblk);
		if (ret < 0)
			goto out;

		/* XXX not verifying this */
		cmt = block_data_buf(cblk);

		dtracef("bstore_find_stable",
			"start %llu mid %llu end %llu gr %llu lba %llu cmt type %u ctr %llu",
			start, mid, end, greatest, lba, cmt->hdr.type,
			le64_to_cpu(cmt->commit_ctr));

		/* so far only trimming at format, 0s are unwritten tail */
		if (cmt->hdr.type == RPDFS_DEV_BLOCK_TYPE_UNINIT) {
			if (lba == 0) {
				/* format should have written to first commit block */
				ret = -EINVAL;
				goto out;
			}
			end = lba - 1;
			continue;
		}

		/* record greatest, continuing search for greater */
		ctr = le64_to_cpu(cmt->commit_ctr);
		if (ctr > greatest) {
			greatest = ctr;
			if (ctr == U64_MAX)
				break;
			start = ctr + 1;

		} else if (ctr >= start) {
			start = ctr + 1;
		}

		/* limit to last possible assuming we read smallest */
		last = ctr + min(U64_MAX - ctr, inst->commit_blocks - 1);
		if (last < end)
			end = last;
	}

	ret = check_complete_commit(inst, greatest);
	if (ret < 0 && greatest > 0)
		ret = check_complete_commit(inst, --greatest);

out:
	*commit_ctr_ret = greatest;
	block_put(cblk);

	dtracef("bstore_find_stable_commit", "ctr %llu ret %d", greatest, ret);
	return ret;
}

/*
 * Init the inst with the device layout.  We get the layout from the
 * first few commits that must have been written by formatting the
 * device.
 */
static int init_journal(struct bstore_instance *inst)
{
	struct rpdfs_dev_commit_block *cmt;
	struct cached_block *cblk = NULL;
	u64 journal_blocks;
	u64 summary_blocks;
	u64 details_blocks;
	u64 total;
	int ret;

	/* no trimming yet so we can always read the first few commit blocks */
	ret = read_block_hdr(inst, 0, RPDFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
	if (ret < 0)
		ret = read_block_hdr(inst, 1, RPDFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
	if (ret < 0)
		goto out;

	cmt = block_data_buf(cblk);

	dtracef("bstore_init_journal", "layout cmt %llu journ %llu sum %llu det %llu stor %llu",
		le64_to_cpu(cmt->layout.commit_blocks), le64_to_cpu(cmt->layout.journal_blocks),
		le64_to_cpu(cmt->layout.summary_blocks), le64_to_cpu(cmt->layout.details_blocks),
		le64_to_cpu(cmt->layout.storage_blocks));

	/* catch initialized zero blocks :/ */
	if (cmt->layout.storage_blocks == 0) {
		ret = -EINVAL;
		goto out;
	}

	inst->dev_uuid = cmt->hdr.dev_uuid;
	inst->have_uuid = true;
	inst->commit_blocks = le64_to_cpu(cmt->layout.commit_blocks);
	journal_blocks = le64_to_cpu(cmt->layout.journal_blocks);
	summary_blocks = le64_to_cpu(cmt->layout.summary_blocks);
	details_blocks = le64_to_cpu(cmt->layout.details_blocks);
	inst->storage_blocks = le64_to_cpu(cmt->layout.storage_blocks);

	total = 0;
	if (check_add_overflow(inst->commit_blocks, journal_blocks, &total) ||
	    check_add_overflow(summary_blocks, total, &total) ||
	    check_add_overflow(details_blocks, total, &total) ||
	    check_add_overflow(inst->storage_blocks, total, &total) ||
	    total > block_total_blocks()) {
		ret = -EINVAL;
		goto out;
	}

	/* arbitrary tiny mins for commits/journal, ulong max for the size of the stable_ht */
	if (inst->commit_blocks < RPDFS_DEV_MIN_JC_BLOCKS					||
	    journal_blocks < RPDFS_DEV_MIN_JC_BLOCKS						||
	    journal_blocks >= ULONG_MAX								||
	    summary_blocks < DIV_ROUND_UP(details_blocks, RPDFS_DEV_SUMMARIES_PER_BLOCK)	||
	    details_blocks < DIV_ROUND_UP(inst->storage_blocks, RPDFS_DEV_DETAILS_PER_BLOCK)) {
		ret = -EINVAL;
		goto out;
	}

	inst->stable_ht = htable_alloc(journal_blocks);
	if (!inst->stable_ht) {
		ret = -ENOMEM;
		goto out;
	}

	inst->journal_lba = inst->commit_blocks;
	inst->summary_lba = inst->journal_lba + journal_blocks;
	inst->details_lba = inst->summary_lba + summary_blocks;
	inst->storage_lba = inst->details_lba + details_blocks;

	ret = 0;
out:
	block_put(cblk);
	return ret;
}

/*
 * Load all the live commits into memory.  This is mostly updating the
 * stable hash table for the final location of blocks in journal.  But
 * we also save the header of the most recent commit.
 */
static int load_commit_blocks(struct bstore_instance *inst, u64 commit_ctr)
{
	struct rpdfs_dev_commit_block *cmt;
	struct cached_block *cblk;
	u64 lba;
	u64 nr;
	int ret;

	lba = commit_ctr_lba(inst, commit_ctr);
	ret = read_block_hdr(inst, lba, RPDFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
	if (ret < 0)
		goto out;

	cmt = block_data_buf(cblk);
	inst->stable_cmt = *cmt;
	nr = le64_to_cpu(cmt->oldest_commit_ctr);
	block_put(cblk);

	dtracef("bstore_load_commit_blocks", "old_ctr %llu ctr %llu", nr, commit_ctr);

	for (; nr <= commit_ctr; nr++) {
		lba = commit_ctr_lba(inst, nr);

		readahead_batch(lba, 16, inst->journal_lba, 1);

		ret = read_block_hdr(inst, lba, RPDFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
		if (ret < 0)
			goto out;

		cmt = block_data_buf(cblk);
		update_commit_lbas(inst, cmt);
		block_put(cblk);
	}

	ret = 0;
out:
	return ret;
}

static int load_summary_block(struct bstore_instance *inst, u64 lba)
{
	struct rpdfs_dev_summary_block *sblk;
	struct bnr_lba_mapping map;
	struct cached_block *cblk;
	int i;
	int ret;

	map_summary_lba(inst, &map, lba);

	ret = read_block_hdr(inst, stable_lba(inst, lba), RPDFS_DEV_BLOCK_TYPE_SUMMARY, &cblk);
	if (ret < 0)
		goto out;

	sblk = block_data_buf(cblk);
	for (i = 0; i < RPDFS_DEV_SUMMARIES_PER_BLOCK; i++) {
		if (sblk->summaries[i].alloc_count != RPDFS_DEV_DETAILS_PER_BLOCK)
			set_free_count(map.bnr + (i * RPDFS_DEV_DETAILS_PER_BLOCK * inst->nr_devds),
				       sblk->summaries[i].alloc_count);

		inst->total_inodes += sblk->summaries[i].nr_inodes;
	}
	block_put(cblk);

	ret = 0;
out:
	return ret;
}

static int load_summary_blocks(struct bstore_instance *inst)
{
	u64 lba;
	int ret = 0;

	for (lba = inst->summary_lba; lba < inst->details_lba; lba++) {
		readahead_batch(lba, 16, inst->details_lba, 1);

		ret = load_summary_block(inst, lba);
		if (ret < 0)
			break;
	}

	return ret;
}

/*
 * The static configuration of the free map stripes will go away when we
 * have dynamic quorum updates.
 */
int bstore_init(u64 nr_devds, u64 this_devd_pos)
{
	struct bstore_instance *inst = &global_bstore_inst;
	u64 commit_ctr;
	int ret;

	inst->nr_devds = nr_devds;
	inst->this_devd_pos = this_devd_pos;

	utask_init_wait_queue(&inst->commit_wq);
	utask_init_wait_queue(&inst->replay_wq);

	inst->dirty_ht = htable_alloc(RPDFS_DEV_COMMIT_MAX_ENTRIES);
	inst->replay_ht = htable_alloc(RPDFS_DEV_COMMIT_MAX_ENTRIES);
	if (!inst->dirty_ht || !inst->replay_ht) {
		free(inst->dirty_ht);
		ret = -ENOMEM;
		goto out;
	}

	/* the bnrs tracked by free-map need to be the first in detail blocks */
	BUILD_BUG_ON(RPDFS_DEV_DETAILS_PER_BLOCK != RPDFS_MSG_BLOCKS_PER_FREE_STRIPE);

	ret = init_journal(inst) ?:
	      free_map_init(inst->storage_blocks, RPDFS_DEV_DETAILS_PER_BLOCK, nr_devds,
			    this_devd_pos) ?:
	      find_stable_commit(inst, &commit_ctr) ?:
	      load_commit_blocks(inst, commit_ctr) ?:
	      load_summary_blocks(inst) ?:
	      utask_create_nowake(commit_write_utask, inst, &inst->commit_tsk) ?:
	      utask_create_nowake(journal_replay_utask, inst, &inst->replay_tsk);
out:
	if (ret < 0)
		bstore_exit();
	return ret;
}

void bstore_exit(void)
{
	struct bstore_instance *inst = &global_bstore_inst;

	utask_destroy(inst->replay_tsk);
	utask_destroy(inst->commit_tsk);
	free(inst->stable_ht);
	free(inst->dirty_ht);
	free(inst->replay_ht);
	memset(inst, 0, sizeof(struct bstore_instance));
}
