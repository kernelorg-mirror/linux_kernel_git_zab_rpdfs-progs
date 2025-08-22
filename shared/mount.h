/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_MOUNT_H
#define RPDFS_SHARED_MOUNT_H

#include "shared/fs_info.h"

int rpdfs_mount(struct rpdfs_fs_info *nfi, int argc, char **argv);
void rpdfs_unmount(struct rpdfs_fs_info *nfi);

#endif
