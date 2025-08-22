/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DEVD_BSTORE_H
#define RPDFS_DEVD_BSTORE_H

#include "utask/block.h"

int bstore_read(u64 dev_bnr, struct cached_block **cblk);
int bstore_write(u64 dev_bnr, struct page *data_page);

int bstore_init(void);
void bstore_exit(void);

#endif
