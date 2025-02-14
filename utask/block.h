/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UTASK_BLOCK_H__
#define __UTASK_BLOCK_H__

#include "shared/lk/byteorder.h"
#include "shared/lk/gfp.h"

struct cached_block;

int block_read(u64 bnr, struct cached_block **cblk_ret);
int block_modify(u64 bnr, struct cached_block **cblk_ret);
int block_overwrite(u64 bnr, struct page *data_page, struct cached_block **cblk_ret);
int block_flush(struct cached_block *cblk);
void *block_data_buf(struct cached_block *cblk);
struct page *block_data_page(struct cached_block *cblk);
void block_put(struct cached_block *cblk);

int block_init(char *dev_path, unsigned long queue_depth);
void block_exit(void);

#endif
