/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_FS_INFO_H
#define RPDFS_SHARED_FS_INFO_H

/*
 * The _fs_info struct is the global system context reference.  Each layer has its
 * info per-system info stored here.
 */
struct rpdfs_block_info;
struct rpdfs_manifest_info;
struct rpdfs_msg_info;

struct rpdfs_fs_info {
	struct rpdfs_block_info *block_info;
	struct rpdfs_manifest_info *manifest_info;
	struct rpdfs_msg_info *msg_info;
};

#define INIT_RPDFS_FS_INFO { NULL, }

#endif
