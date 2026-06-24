/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DEVD_LSTORE_H
#define RPDFS_DEVD_LSTORE_H

#include "shared/format-dev-log.h"
#include "shared/format-msg.h"

#include "utask/blk.h"

struct blk_handle *lstore_alloc_dirty(u8 type, __le64 crc, u64 *dev_addr);
struct blk_handle *lstore_read_block(u64 dev_addr, u8 type, __le64 crc, struct blk_ticket *tkt);

struct blk_handle *lstore_read(struct rpdfs_block_key *key, struct rpdfs_msg_block_details *det);
int lstore_write(struct rpdfs_block_key *key, struct page *data_page, size_t size,
		 struct rpdfs_msg_block_details *det);
void lstore_get_block_counts(struct rpdfs_msg_block_counts_result *bcr);

int lstore_init(char *dev_path);
void lstore_exit(void);

#endif
