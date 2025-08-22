/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_DATA_H
#define RPDFS_SHARED_DATA_H

#include "shared/fs_info.h"
#include "shared/inode.h"

/*
 * The maximum amount of data we want to read/write in one transaction.
 */
#define RPDFS_DATA_MAX_IO (16 * RPDFS_BLOCK_SIZE)

ssize_t rpdfs_data_read(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *ig,
			u64 offset, void *buf, size_t len);
ssize_t rpdfs_data_write(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *ig,
			 u64 offset, void *buf, size_t len);

#endif
