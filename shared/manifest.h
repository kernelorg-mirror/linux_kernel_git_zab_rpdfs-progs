/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_MANIFEST_H
#define RPDFS_SHARED_MANIFEST_H

#include "shared/lk/in.h"
#include "shared/lk/list.h"

#include "shared/fs_info.h"

struct rpdfs_manifest_addr_head {
	struct list_head head;
	struct sockaddr_in addr;
};

int rpdfs_manifest_map_block(struct rpdfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr);
int rpdfs_manifest_setup(struct rpdfs_fs_info *nfi, struct list_head *list, u8 nr);
void rpdfs_manifest_destroy(struct rpdfs_fs_info *nfi);

#endif
