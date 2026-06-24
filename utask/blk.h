/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UTASK_BLK_H__
#define __UTASK_BLK_H__

#include "shared/lk/byteorder.h"
#include "shared/lk/gfp.h"
#include "shared/lk/list.h"

#include "shared/format-msg.h"

struct blk_handle {
	struct rpdfs_block_key key;
	void *private;
	void *data;
	u32 size;
	u32 verified:1;
};

/*
 * Tickets protect blocks from being reclaimed.  Tasks that want to pin
 * blocks pass an open ticket to blk_get.  Until they close the ticket
 * those blocks won't be freed.
 *
 * This is implemented by moving blocks to the end of the lru and
 * marking them with the ticket number when they're accessed without any
 * ticket protection.  Shrinking stops when it hits a block protected by
 * the oldest open ticket.
 *
 * This can mean that writes don't have to use tickets.  All the input
 * blocks can be prepared with a ticket that's held across the
 * non-blocking write modification of dirty blocks.  All that's left is
 * allocating new blocks and those will go the end of the lru.
 * Shrinking won't be able to find them while the writer has their
 * ticket.
 */
struct blk_ticket {
	struct list_head head;
	u64 number;
};

/*
 * The cleanup attribute ensures that tickets will be closed as their
 * declaring scope closes.
 */
#define DECLARE_BLK_TICKET(name) \
	__attribute__ ((__cleanup__(blk_close_ticket))) \
	struct blk_ticket name = (struct blk_ticket) { .head = LIST_HEAD_INIT(name.head) }

typedef enum {
	/*
	 * Always insert a newly allocated block in the cache and return
	 * it without initializing its contents.  Any existing block in
	 * the hash is freed.  The caller is responsible for
	 * initializing the block's contents.
	 */
	BGF_NEW		= (1 << 0),

	/*
	 * Don't allocate missing blocks, only return existing blocks.
	 * -ENOENT is returned if a cached block wasn't found at the
	 * key.
	 */
	BGF_NOALLOC	= (1 << 1),
} bgf_t;

struct blk_ops {
	int (*read)(struct rpdfs_block_key *key, struct page *data_page);
	int (*write)(struct list_head *list);
};

void blk_open_ticket(struct blk_ticket *tkt);
void blk_close_ticket(struct blk_ticket *tkt);

struct blk_handle *blk_get(struct rpdfs_block_key *key, bgf_t bgf, struct blk_ticket *tkt);
void blk_change_key(struct blk_handle *hnd, struct rpdfs_block_key *key);
void blk_set_data_page(struct blk_handle *hnd, struct page *data_page);
struct page *blk_data_page(struct blk_handle *hnd);
void blk_mark_dirty(struct blk_handle *hnd);
bool blk_can_modify(struct blk_handle *hnd);
void blk_schedule_write_dirty(void);
struct blk_handle *blk_first_dirty_handle(struct list_head *list);
struct blk_handle *blk_next_dirty_handle(struct blk_handle *hnd, struct list_head *list);

int blk_init(struct blk_ops *ops);
void blk_exit(void);

#endif
