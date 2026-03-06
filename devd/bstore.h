/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DEVD_BSTORE_H
#define RPDFS_DEVD_BSTORE_H

#include "shared/format-dev.h"
#include "shared/format-msg.h"

#include "utask/block.h"

int bstore_read(u64 dev_bnr, struct cached_block **cblk, struct rpdfs_block_details *det);
int bstore_write(u64 dev_bnr, struct page *data_page, struct rpdfs_block_details *in_det);
int bstore_get_free_details(u64 bnr, unsigned long *bmap,
			    struct rpdfs_msg_free_stripe_detail *fsd, size_t size);
u64 bstore_contig_devd_block_bnr_distance(void);

int bstore_init(u64 nr_devds, u64 this_devd_pos);
void bstore_exit(void);

#endif
