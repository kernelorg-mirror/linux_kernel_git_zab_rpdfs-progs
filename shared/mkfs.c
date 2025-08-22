/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/ktime.h"
#include "shared/lk/stat.h"

#include "shared/format-block.h"
#include "shared/fs_info.h"
#include "shared/inode.h"
#include "shared/mkfs.h"
#include "shared/txn.h"

/*
 * This only creates the inode for the root directory.  It will need to
 * initialize allocation blocks and that means needing to see the size
 * of the volume.
 */
int rpdfs_mkfs(struct rpdfs_fs_info *nfi)
{
	struct rpdfs_transaction txn;
	struct rpdfs_inode_txn_ref itref;
	struct rpdfs_inode_ino_gen ig = INIT_RPDFS_ROOT_IG;
	u64 nsec;
	int ret;

	rpdfs_txn_init(&txn);

	do {
		nsec = ktime_to_ns(ktime_get_real());

		ret = rpdfs_inode_get(nfi, &txn, NBF_WRITE, &ig, &itref)			?:
		      rpdfs_inode_init(&itref, &ig, 2, S_IFDIR | 0755, nsec, &ig);

	} while (rpdfs_txn_retry(nfi, &txn, &ret));

	rpdfs_txn_teardown(nfi, &txn);

	return ret;
}
