/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_XATTR_H
#define RPDFS_SHARED_XATTR_H

#include "shared/fs_info.h"
#include "shared/inode.h"

int rpdfs_xattr_get(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *ig, char *name,
			void *value, size_t val_size);
int rpdfs_xattr_remove(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *ig, char *name);
int rpdfs_xattr_set(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *ig, char *name,
		    void *value, size_t val_size, int flags);
int rpdfs_xattr_list(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *ig, void *buf,
		     size_t size);

#endif
