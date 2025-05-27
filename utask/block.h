/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UTASK_BLOCK_H__
#define __UTASK_BLOCK_H__

#include "shared/lk/byteorder.h"
#include "shared/lk/gfp.h"
#include "shared/lk/list.h"

struct cached_block;

int block_lookup(u64 bnr, struct cached_block **cblk_ret);
int block_read(u64 bnr, struct cached_block **cblk_ret);
void block_readahead(u64 bnr);
void *block_data_buf(struct cached_block *cblk);
struct page *block_data_page(struct cached_block *cblk);
void block_invalidate(struct cached_block *cblk);
void block_put(struct cached_block *cblk);
void block_putp(struct cached_block **cblk);
u64 block_total_blocks(void);

int block_write_all_dirty(void);
void block_clean_all_dirty(void);

int block_alloc_pool(struct list_head *pool, size_t nr);
void block_free_pool(struct list_head *pool);
int block_create_dirty(u64 bnr, struct list_head *pool, struct page *data_page,
		       struct cached_block **cblk_ret);

int block_init(char *dev_path, unsigned long queue_depth);
void block_exit(void);

#endif
