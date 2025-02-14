/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* O_DIRECT */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "shared/lk/list.h"
#include "shared/lk/rbtree.h"

#include "shared/format-block.h"

#include "utask/block.h"
#include "utask/utask.h"

/*
 * This provides a block cache on top of local storage for tasks in our
 * utask runtime, via IO managed by io_uring.
 *
 * There can only be one utask executing here at a time, letting us
 * avoid all the complexity of concurrent programming.  (Though as we
 * block the next utask can call in -- it's still re-entrant).
 *
 * Interestingly, callers of this cache won't leave behind dirty blocks.
 * They're not making many incremental changes that need to have
 * writeback caching.  They're processing in large batches or are making
 * persistence promises before they can reply to network messages.
 *
 * XXX of course, the callers and interface don't implement this quite
 * yet.  It'll come oneline as we build out the atomic transactions in
 * the networking protocol and the block journaling layer in devd.
 */

static struct block_cache_instance {
	struct rb_root block_root;
	struct list_head submit_list;
	struct list_head lru_list;
	struct utask *submit_tsk;
	struct utask_wait_queue submit_wq;
	struct utask *shrink_tsk;
	struct utask_wait_queue shrink_wq;
	unsigned long nr_allocated;
	unsigned long nr_in_flight;
	unsigned long queue_depth;
	int dev_fd;

} global_block_cache_inst = {
	.block_root = RB_ROOT,
	.submit_list = LIST_HEAD_INIT(global_block_cache_inst.submit_list),
	.lru_list = LIST_HEAD_INIT(global_block_cache_inst.lru_list),
	.submit_wq = INIT_UTASK_WAIT_QUEUE(global_block_cache_inst.submit_wq),
	.shrink_wq = INIT_UTASK_WAIT_QUEUE(global_block_cache_inst.shrink_wq),
	.dev_fd = -1,
};

struct cached_block {
	struct rb_node node;
	struct list_head submit_head;
	struct list_head lru_head;
	struct utask_wait_queue wq;
	struct utask_cqe_callback cb;
	struct page *data_page;
	u64 bnr;
	unsigned long readers;
	int error;
	unsigned uptodate:1,
		 flushing:1,
		 queued:1,
		 writer:1;
};

static struct cached_block *alloc_cblk(struct block_cache_instance *inst)
{
	struct cached_block *cblk;

	cblk = calloc(1, sizeof(struct cached_block));
	if (!cblk || !(cblk->data_page = alloc_page(GFP_NOFS))) {
		free(cblk);
		return NULL;
	}

	INIT_LIST_HEAD(&cblk->submit_head);
	INIT_LIST_HEAD(&cblk->lru_head);
	utask_init_wait_queue(&cblk->wq);

	inst->nr_allocated++;

	return cblk;
}

static void free_cblk(struct block_cache_instance *inst, struct cached_block *cblk)
{
	if (!RB_EMPTY_NODE(&cblk->node))
		rb_erase(&cblk->node, &inst->block_root);
	if (!list_empty(&cblk->submit_head))
		list_del_init(&cblk->submit_head);
	if (!list_empty(&cblk->lru_head))
		list_del_init(&cblk->lru_head);

	BUG_ON(utask_waitqueue_active(&cblk->wq));

	__free_page(cblk->data_page);
	free(cblk);

	inst->nr_allocated--;

}

/*
 * XXX Until configurable, we chose an arbitrary limit on the cache size
 * that seems not tiny but also won't overwhelm guests with small
 * amounts of memory and a handful of processes.
 */
#define MAX_CACHED_BLOCKS	(256 * 1024 * 1024 / NGNFS_BLOCK_SIZE)
static bool should_shrink(struct block_cache_instance *inst)
{
	return inst->nr_allocated > MAX_CACHED_BLOCKS && !list_empty(&inst->lru_list);
}

static void shrink_utask(void *data)
{
	struct block_cache_instance *inst = data;
	struct cached_block *cblk;
	struct cached_block *tmp;

	for (;;) {
		utask_wait_event(&inst->shrink_wq, should_shrink(inst));

		list_for_each_entry_safe(cblk, tmp, &inst->lru_list, lru_head) {
			if (inst->nr_allocated < MAX_CACHED_BLOCKS)
				break;

			free_cblk(inst, cblk);
		}
	}
}

/*
 * We only put blocks that aren't actively in use on the LRU.
 */
static void update_lru(struct block_cache_instance *inst, struct cached_block *cblk)
{
	bool on_lru = !list_empty(&cblk->lru_head);
	bool idle = !utask_waitqueue_active(&cblk->wq) && cblk->readers == 0 && !cblk->writer &&
		    !cblk->queued;

	if (idle && !on_lru)
		list_add_tail(&cblk->lru_head, &inst->lru_list);
	else if (!idle && on_lru)
		list_del_init(&cblk->lru_head);

	if (should_shrink(inst))
		utask_wake_task(inst->shrink_tsk);
}

/*
 * Acquire a read or write reference to a cached block.  While we have a
 * reference the block will not be found on the LRU and won't be freed.
 * A successfully returned reference must be released with block_put().
 */
enum {
	GCB_ALLOC = (1 << 0),
	GCB_WRITER = (1 << 1),
};
static struct cached_block *get_cblk(struct block_cache_instance *inst, u64 bnr, int gcb)
{
	struct rb_node **node = &inst->block_root.rb_node;
	struct cached_block *cblk = NULL;
	struct rb_node *parent = NULL;

	while (*node) {
		parent = *node;
		cblk = container_of(*node, struct cached_block, node);
		if (bnr == cblk->bnr)
			break;
		if (bnr < cblk->bnr)
			node = &(*node)->rb_left;
		else
			node = &(*node)->rb_right;
		cblk = NULL;
	}

	if (!cblk && (gcb & GCB_ALLOC)) {
		cblk = alloc_cblk(inst);
		if (cblk) {
			cblk->bnr = bnr;

			rb_link_node(&cblk->node, parent, node);
			rb_insert_color(&cblk->node, &inst->block_root);
		}
	}

	if (cblk) {
		utask_wait_event(&cblk->wq,
			         !cblk->writer && (!(gcb & GCB_WRITER) || !cblk->readers));
		if (gcb & GCB_WRITER)
			cblk->writer = 1;
		else
			cblk->readers++;

		update_lru(inst, cblk);
	}

	return cblk;
}

static void put_cblk(struct block_cache_instance *inst, struct cached_block *cblk)
{
	if (cblk) {
		/* XXX not thrilled with this.. but.. */
		if (cblk->writer)
			cblk->writer = 0;
		else
			cblk->readers--;

		update_lru(inst, cblk);
		utask_wake_all(&cblk->wq);
	}
}

static bool should_submit(struct block_cache_instance *inst)
{
	return inst->nr_in_flight < inst->queue_depth && !list_empty(&inst->submit_list);
}

/*
 * We're processing the completion in the cqe callback itself, rather
 * than trying to hand it off to a utask.  It seems like it'd be cleaner
 * to have the work done in a utask (that could conceivably have
 * accounting), but it also seems like busywork for no functional gain.
 */
static void io_completion(struct io_uring_cqe *cqe, struct utask_cqe_callback *cb)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk = container_of(cb, struct cached_block, cb);
	int err;

	/* XXX error handling :) */
	BUG_ON(cqe->res != NGNFS_BLOCK_SIZE);
	err = 0;

	if (cblk->flushing)
		cblk->flushing = 0;
	else if (!err)
		cblk->uptodate = 1;

	cblk->queued = 0;
	cblk->error = err;

	update_lru(inst, cblk);
	utask_wake_all(&cblk->wq);

	inst->nr_in_flight--;
	if (should_submit(inst))
		utask_wake_task(inst->submit_tsk);

	/* catch block_exit shutting down */
	if (inst->queue_depth == 0 && inst->nr_in_flight == 0)
		utask_wake_all(&inst->submit_wq);
}

/*
 * Prepare queued IO for submitting to io_uring.  The IO queue is
 * currently a simple list but this is where we'd add any sort of io
 * sorting/priority.
 */
static void submit_utask(void *data)
{
	struct block_cache_instance *inst = data;
	struct io_uring_sqe *sqe;
	struct cached_block *cblk;
	struct cached_block *tmp;

	for (;;) {
		utask_wait_event(&inst->submit_wq, should_submit(inst));

		list_for_each_entry_safe(cblk, tmp, &inst->submit_list, submit_head) {
			if (inst->nr_in_flight >= inst->queue_depth)
				break;

			sqe = io_uring_get_sqe(utask_ring());
			if (!sqe)
				break;

			utask_set_sqe_callback(sqe, &cblk->cb, io_completion);
			if (cblk->flushing)
				io_uring_prep_write(sqe, inst->dev_fd,
						    page_address(cblk->data_page),
						    NGNFS_BLOCK_SIZE,
						    cblk->bnr << NGNFS_BLOCK_SHIFT);
			else
				io_uring_prep_read(sqe, inst->dev_fd,
						   page_address(cblk->data_page),
						   NGNFS_BLOCK_SIZE,
						   cblk->bnr << NGNFS_BLOCK_SHIFT);

			inst->nr_in_flight++;
			list_del_init(&cblk->submit_head);
		}
	}
}

/*
 * Return once the block is uptodate or has an error.  It's queued if it
 * wasn't already.
 */
static int queue_and_wait_for_uptodate(struct block_cache_instance *inst,
				       struct cached_block *cblk)
{
	if (!cblk->error && !cblk->uptodate && !cblk->queued) {
		list_add_tail(&cblk->submit_head, &inst->submit_list);
		cblk->queued = 1;
		if (should_submit(inst))
			utask_wake_task(inst->submit_tsk);
	}

	utask_wait_event(&cblk->wq, cblk->uptodate || cblk->error);

	return cblk->error;
}

int block_read(u64 bnr, struct cached_block **cblk_ret)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	int ret;

	cblk = get_cblk(inst, bnr, GCB_ALLOC);
	if (!cblk) {
		ret = -ENOMEM;
		goto out;
	}

	ret = queue_and_wait_for_uptodate(inst, cblk);
out:
	if (ret < 0) {
		put_cblk(inst, cblk);
		cblk = NULL;
	}

	*cblk_ret = cblk;
	return ret;
}

/*
 * Return a write reference to block after reading its current contents.
 */
int block_modify(u64 bnr, struct cached_block **cblk_ret)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	int ret;

	cblk = get_cblk(inst, bnr, GCB_ALLOC | GCB_WRITER);
	if (!cblk) {
		ret = -ENOMEM;
		goto out;
	}

	ret = queue_and_wait_for_uptodate(inst, cblk);
	if (ret == 0)
		utask_wait_event(&cblk->wq, !cblk->flushing);
out:
	if (ret < 0) {
		put_cblk(inst, cblk);
		cblk = NULL;
	}

	*cblk_ret = cblk;
	return ret;
}

/*
 * Return a write reference to block regardless of its current contents.
 */
int block_overwrite(u64 bnr, struct page *data_page, struct cached_block **cblk_ret)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	int ret;

	cblk = get_cblk(inst, bnr, GCB_ALLOC | GCB_WRITER);
	if (!cblk) {
		ret = -ENOMEM;
		goto out;
	}

	utask_wait_event(&cblk->wq, !cblk->flushing);

	if (data_page) {
		if (cblk->data_page)
			put_page(cblk->data_page);
		cblk->data_page = data_page;
		get_page(cblk->data_page);
	}

	cblk->uptodate = 1;
	cblk->error = 0;

	ret = 0;
out:
	if (ret < 0) {
		put_cblk(inst, cblk);
		cblk = NULL;
	}

	*cblk_ret = cblk;
	return ret;
}

/*
 * An interim measure until we get shared transactions that track blocks
 * that need to be written.  The block is only "flushing" from when we
 * start IO to when it completes.  The caller must have a write ref.
 */
int block_flush(struct cached_block *cblk)
{
	struct block_cache_instance *inst = &global_block_cache_inst;

	utask_wait_event(&cblk->wq, !cblk->flushing && !cblk->queued);

	cblk->flushing = 1;
	list_add_tail(&cblk->submit_head, &inst->submit_list);
	cblk->queued = 1;
	if (should_submit(inst))
		utask_wake_task(inst->submit_tsk);

	utask_wait_event(&cblk->wq, !cblk->flushing);

	return cblk->error;
}

void *block_data_buf(struct cached_block *cblk)
{
	return page_address(cblk->data_page);
}

struct page *block_data_page(struct cached_block *cblk)
{
	return cblk->data_page;
}

void block_put(struct cached_block *cblk)
{
	struct block_cache_instance *inst = &global_block_cache_inst;

	if (cblk) {
		/* XXX not thrilled with this assumption that these refs are ours */
		if (cblk->writer)
			cblk->writer = 0;
		else
			cblk->readers--;

		update_lru(inst, cblk);
		utask_wake_all(&cblk->wq);
	}
}

int block_init(char *dev_path, unsigned long queue_depth)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	int oflags;
	int ret;
	int fd;

	if (WARN_ON_ONCE(queue_depth == 0))
		return -EINVAL;

	oflags = O_RDWR | O_DIRECT;
	fd = open(dev_path, oflags, O_RDWR);
	if (fd < 0 && errno == EINVAL) {
		oflags &= ~O_DIRECT;
		errno = 0;
		fd = open(dev_path, oflags, O_RDWR);
		if (fd >= 0)
			log("O_DIRECT not supported on '%s', using buffered", dev_path);
	}
	if (fd < 0) {
		ret = -errno;
		log("error opening device '%s' :" ENOF, dev_path, ENOA(-ret));
		goto out;
	}
	inst->queue_depth = queue_depth;
	inst->dev_fd = fd;

	ret = utask_create(submit_utask, inst, &inst->submit_tsk) ?:
	      utask_create(shrink_utask, inst, &inst->shrink_tsk);
	if (ret < 0)
		block_exit();
out:
	return ret;
}

void block_exit(void)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	struct cached_block *tmp;

	/* stop submission and wait for ios to drain */
	inst->queue_depth = 0;
	utask_wait_event(&inst->submit_wq, inst->nr_in_flight == 0);

	utask_destroy(inst->submit_tsk);
	utask_destroy(inst->shrink_tsk);

	if (inst->dev_fd >= 0)
		close(inst->dev_fd);

	rbtree_postorder_for_each_entry_safe(cblk, tmp, &inst->block_root, node)
		free_cblk(inst, cblk);

	inst->block_root = RB_ROOT;
	inst->submit_tsk = NULL;
	inst->shrink_tsk = NULL;
	inst->dev_fd = -1;
}
