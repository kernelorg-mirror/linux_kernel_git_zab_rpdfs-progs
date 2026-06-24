/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <errno.h>
#include "shared/lk/err.h"
#include "shared/dtracef.h"
#include "shared/hash_table.h"
#include "shared/log.h"
#include "utask/blk.h"
#include "utask/utask.h"

/*
 * A simple block cache for our utask environment.
 *
 * Blocks are identified by a large key whose meaning is up to the
 * caller.  Ops provide the mechanism for IO transfer while this handles
 * the state transitions around reading and dirtying.
 *
 * This uses per-task tickets to protect against shrinking rather than
 * per-block refcounts.  The safety of read and write accesses is the
 * responsibility of the caller.
 */

static struct blk_instance {
	struct blk_ops *ops;
	struct hash_table *ht;
	struct utask *write_dirty_tsk;
	struct list_head tickets;
	u64 next_ticket;
	struct list_head dirty_list; /* careful, also has writer entries on the list */
	struct list_head lru_list;
	size_t nr_blocks;
	size_t shrink_limit;
} global_blk_inst;

struct blk {
	struct page *data_page;
	struct list_head dirty_head;
	struct list_head lru_head;
	struct utask_wait_queue waitq;
	u64 accessed;
	int error;
	unsigned dirty:1,
		 reading:1,
		 uptodate:1,
		 writeback:1;
	struct blk_handle hnd;
};

#define BF	"blk %p hnd %p pg %p k "RBKF" acc %llu d %u r %u u %u v %u"
#define BA(blk)	(blk), &(blk)->hnd, (blk)->data_page, RBKA(&(blk)->hnd.key), \
		(blk)->accessed, (blk)->dirty, (blk)->reading, (blk)->uptodate, (blk)->hnd.verified

static struct blk *hnd_container(struct blk_handle *hnd)
{
	return container_of(hnd, struct blk, hnd);
}

static void mark_accessed(struct blk_instance *inst, struct blk *blk, struct blk_ticket *tkt)
{
	if (blk->accessed < tkt->number) {
		blk->accessed = tkt->number;
		list_move_tail(&blk->lru_head, &inst->lru_list);
	}
}

static u64 oldest_ticket(struct blk_instance *inst)
{
	struct blk_ticket *tkt;

	if (list_empty(&inst->tickets))
		return inst->next_ticket - 1;

	tkt = list_first_entry_or_null(&inst->tickets, struct blk_ticket, head);
	return tkt->number;
}

static struct blk *alloc_blk(struct blk_instance *inst)
{
	struct blk *blk;

	blk = calloc(1, sizeof(struct blk));
	if (blk) {
		blk->data_page = alloc_page(GFP_NOFS);
		if (blk->data_page) {
			INIT_LIST_HEAD(&blk->dirty_head);
			list_add_tail(&blk->lru_head, &inst->lru_list);
			inst->nr_blocks++;
			utask_init_wait_queue(&blk->waitq);
			blk->hnd.data = page_address(blk->data_page);
		} else {
			kfree(blk);
			blk = NULL;
		}
	}

	return blk;
}

static void free_blk(struct blk_instance *inst, struct blk *blk, bool delete)
{
	if (!IS_ERR_OR_NULL(blk)) {
		if (delete)
			htable_delete(inst->ht, &blk->hnd.key);
		if (!list_empty(&blk->dirty_head))
			list_del_init(&blk->dirty_head);
		list_del(&blk->lru_head);
		inst->nr_blocks--;
		dtracef("blk_free", BF, BA(blk));
		__free_page(blk->data_page);
		free(blk->hnd.private);
		free(blk);
	}
}

/*
 * Once we run we take all the current dirty blocks off the dirty list
 * and give them to the write method.  The caller's coordination between
 * dirtying and their ops write method ensures that the set of blocks
 * represents a coherent atomic change.
 */
static void write_dirty_utask(void *data)
{
	struct blk_instance *inst = data;
	LIST_HEAD(list);
	struct blk *blk;
	struct blk *tmp;
	int ret;

	list_splice_init(&inst->dirty_list, &list);

	list_for_each_entry_safe(blk, tmp, &list, dirty_head) {
		blk->writeback = 1;
	}

	inst->write_dirty_tsk = NULL;
	ret = inst->ops->write(&list);
	BUG_ON(ret != 0);

	list_for_each_entry_safe(blk, tmp, &list, dirty_head) {
		blk->writeback = 0;
		blk->dirty = 0;
		list_del_init(&blk->dirty_head);
	}
}

/*
 * Try to shrink cached blocks if we've exceeded the shrink limit.  We
 * walk from the end of the lru where the oldest accesses are.  When
 * stop when we hit a block that was accessed more recently than the
 * oldest open ticket.
 */
static void try_shrink(struct blk_instance *inst)
{
	struct blk *blk;
	struct blk *tmp;
	u64 oldest;
	s64 nr;

	nr = (s64)inst->nr_blocks - (s64)inst->shrink_limit;
	if (nr <= 0)
		return;

	oldest = oldest_ticket(inst);

	list_for_each_entry_safe(blk, tmp, &inst->lru_list, lru_head) {
		if (blk->accessed >= oldest)
			break;

		if (blk->reading || blk->dirty)
			continue;

		free_blk(inst, blk, true);
		if (--nr <= 0)
			break;
	}
}

/*
 * Give the caller a ticket that will stop reclaim on any blocks that
 * are returned from blk_get while the ticket is still open.  The ticket
 * can be closed explicitly or will be closed as its declaring scope
 * closes.
 *
 * If this is called again without an intervening close then the effects
 * of close are implicit -- any blocks protected by the previous open
 * could be reclaimed.
 */
void blk_open_ticket(struct blk_ticket *tkt)
{
	struct blk_instance *inst = &global_blk_inst;

	if (list_empty(&tkt->head))
		list_add_tail(&tkt->head, &inst->tickets);
	else
		list_move_tail(&tkt->head, &inst->tickets);
	tkt->number = inst->next_ticket++;
}

/*
 * The caller is finished with the ticket.  Any blocks that were
 * protected may not be reclaimed.
 */
void blk_close_ticket(struct blk_ticket *tkt)
{
	if (!list_empty(&tkt->head))
		list_del_init(&tkt->head);
}

/*
 * Give the caller a handle on the cached block identified by the given
 * key.  If the key wasn't present then we block in the ops read method
 * to get its current contents.  Flags can return errors or initialize
 * the block instead of blocking to read.
 */
struct blk_handle *blk_get(struct rpdfs_block_key *key, bgf_t bgf, struct blk_ticket *tkt)
{
	struct blk_instance *inst = &global_blk_inst;
	struct blk_handle *hnd;
	struct blk *blk = NULL;
	int ret;

	blk = htable_lookup(inst->ht, key);
	if (blk && (bgf & BGF_NEW)) {
		ret = utask_wait_event(&blk->waitq, !blk->reading);
		if (ret < 0)
			goto out;
		free_blk(inst, blk, true);
		blk = NULL;
	}

	if (!blk) {
		if (bgf & BGF_NOALLOC) {
			ret = -ENOENT;
			goto out;
		}

		blk = alloc_blk(inst);
		if (!blk) {
			ret = -ENOMEM;
			goto out;
		}

		blk->hnd.key = *key;
		blk->hnd.size = RPDFS_BLOCK_SIZE;

		htable_insert(inst->ht, blk);
		try_shrink(inst);

		if (bgf & BGF_NEW) {
			blk->uptodate = 1;
			blk->hnd.verified = 1;
		}
	}

	if (!blk->uptodate && !blk->reading) {
		blk->reading = 1;
		ret = inst->ops->read(&blk->hnd.key, blk->data_page);
		blk->reading = 0;
		if (ret < 0)
			blk->error = ret;
		else
			blk->uptodate = 1;
		utask_wake_all(&blk->waitq);
		goto out;
	}

	ret = utask_wait_event(&blk->waitq, blk->uptodate || blk->error);
	if (ret == 0 && blk->error)
		ret = blk->error;
out:
	if (ret < 0) {
		hnd = ERR_PTR(ret);
	} else {
		if (tkt)
			mark_accessed(inst, blk, tkt);
		dtracef("blk_get", BF, BA(blk));
		hnd = &blk->hnd;
	}

	return hnd;
}

/*
 * Change a currently cached block to be stored at a different key.
 * This does not examine the state of the block at all, correctness is
 * up to the caller.
 */
void blk_change_key(struct blk_handle *hnd, struct rpdfs_block_key *key)
{
	struct blk_instance *inst = &global_blk_inst;
	struct blk *blk = hnd_container(hnd);

	htable_delete(inst->ht, &blk->hnd.key);
	hnd->key = *key;
	htable_insert(inst->ht, blk);
}

void blk_set_data_page(struct blk_handle *hnd, struct page *data_page)
{
	struct blk *blk = hnd_container(hnd);

	put_page(blk->data_page);
	get_page(data_page);
	blk->data_page = data_page;
	blk->hnd.data = page_address(blk->data_page);
}

void blk_mark_dirty(struct blk_handle *hnd)
{
	struct blk_instance *inst = &global_blk_inst;
	struct blk *blk = hnd_container(hnd);

	if (!blk->dirty) {
		blk->dirty = 1;
		blk->uptodate = 1;
		blk->hnd.verified = 1;
		list_add_tail(&blk->dirty_head, &inst->dirty_list);
		dtracef("blk_mark_dirty", BF, BA(blk));
	}
}

bool blk_can_modify(struct blk_handle *hnd)
{
	struct blk *blk = hnd_container(hnd);

	return blk->dirty && !blk->writeback;
}

/*
 * This schedules the write dirty utask to run after all currently
 * runnable tasks.  When it runs it will pass the current dirty list off
 * to the ops write method.
 */
void blk_schedule_write_dirty(void)
{
	struct blk_instance *inst = &global_blk_inst;
	int ret;

	if (!inst->write_dirty_tsk) {
		ret = utask_create(write_dirty_utask, inst, &inst->write_dirty_tsk);
		BUG_ON(ret < 0);
		utask_destroy_at_finish(inst->write_dirty_tsk);
	}
}

struct blk_handle *blk_first_dirty_handle(struct list_head *list)
{
	struct blk *blk = list_first_entry_or_null(list, struct blk, dirty_head);

	return blk ? &blk->hnd : NULL;
}

struct blk_handle *blk_next_dirty_handle(struct blk_handle *hnd, struct list_head *list)
{
	struct blk *blk = hnd_container(hnd);

	list_for_each_entry_continue(blk, list, dirty_head)
		return &blk->hnd;

	return NULL;
}

int blk_init(struct blk_ops *ops)
{
	struct blk_instance *inst = &global_blk_inst;
	int ret;

	inst->ops = ops;
	INIT_LIST_HEAD(&inst->tickets);
	inst->next_ticket = 1;
	INIT_LIST_HEAD(&inst->dirty_list);
	INIT_LIST_HEAD(&inst->lru_list);
	inst->nr_blocks = 0;
	inst->shrink_limit = (256 * 1024 * 1024) / RPDFS_BLOCK_SIZE; /* *shrug* */

	BUILD_BUG_ON(!__builtin_types_compatible_p(typeof_member(struct blk, hnd.key),
						   struct rpdfs_block_key));

	inst->ht = htable_alloc(offsetof(struct blk, hnd.key), sizeof(struct rpdfs_block_key));
	if (!inst->ht)
		ret = -ENOMEM;
	else
		ret = 0;

	return ret;
}

static void free_htable_blk(void *obj, void *arg)
{
	struct blk *blk = obj;
	struct blk_instance *inst = arg;

	free_blk(inst, blk, false);
}

void blk_exit(void)
{
	struct blk_instance *inst = &global_blk_inst;

	if (inst->ht) {
		htable_destroy(inst->ht, free_htable_blk, inst);
		inst->ht = NULL;
	}
}
