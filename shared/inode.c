/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/errno.h"
#include "shared/lk/kernel.h"
#include "shared/lk/minmax.h"
#include "shared/lk/stat.h"
#include "shared/lk/types.h"

#include "shared/block.h"
#include "shared/format-block.h"
#include "shared/inode.h"
#include "shared/txn.h"

/*
 * For now inode structs are simply stored at the front of blocks.  The
 * block number is the inode number, avoiding false sharing of block
 * access for consecutive inodes.  We're almost certainly going to want
 * to pack inode data into the rest of the block.
 */

/*
 * Most (all?) file systems have a non-zero i_size for a dir containing
 * only "." and "..", so define one that matches how we count i_size for
 * other dentries (name_len plus null terminator). It doesn't really
 * matter.
 */
#define RPDFS_DIR_SIZE	5

int rpdfs_inode_init(struct rpdfs_inode_txn_ref *itref, struct rpdfs_inode_ino_gen *ig, u32 nlink,
		     umode_t mode, u64 nsec, struct rpdfs_inode_ino_gen *parent_ig)
{
	struct rpdfs_txn_block *tblk = itref->tblk;
	struct rpdfs_inode *ninode = itref->ninode;
	u64 i_size;

	if (S_ISDIR(mode))
		i_size = RPDFS_DIR_SIZE;
	else
		i_size = 0;

	rpdfs_tblk_assign(tblk, ninode->ig.ino, cpu_to_le64(ig->ino));
	rpdfs_tblk_assign(tblk, ninode->ig.gen, cpu_to_le64(ig->gen));
	rpdfs_tblk_assign(tblk, ninode->size, cpu_to_le64(i_size));
	rpdfs_tblk_assign(tblk, ninode->version, cpu_to_le64(1));
	rpdfs_tblk_assign(tblk, ninode->parent_ig.ino, cpu_to_le64(parent_ig->ino));
	rpdfs_tblk_assign(tblk, ninode->parent_ig.gen, cpu_to_le64(parent_ig->gen));
	rpdfs_tblk_assign(tblk, ninode->nlink, cpu_to_le32(nlink));
	rpdfs_tblk_assign(tblk, ninode->uid, 0);
	rpdfs_tblk_assign(tblk, ninode->gid, 0);
	rpdfs_tblk_assign(tblk, ninode->mode, cpu_to_le32(mode));
	rpdfs_tblk_assign(tblk, ninode->rdev, 0);
	rpdfs_tblk_assign(tblk, ninode->flags, 0);
	rpdfs_tblk_assign(tblk, ninode->xattr_creates, 0);
	rpdfs_tblk_assign(tblk, ninode->xattr_names_len, 0);
	rpdfs_tblk_assign(tblk, ninode->atime_nsec, cpu_to_le64(nsec));
	rpdfs_tblk_assign(tblk, ninode->ctime_nsec, ninode->atime_nsec);
	rpdfs_tblk_assign(tblk, ninode->mtime_nsec, ninode->atime_nsec);
	rpdfs_tblk_assign(tblk, ninode->crtime_nsec, ninode->atime_nsec);
	rpdfs_tblk_memset(tblk, &ninode->dirents, 0, sizeof(ninode->dirents));
	rpdfs_tblk_memset(tblk, &ninode->xattrs, 0, sizeof(ninode->xattrs));
	rpdfs_tblk_memset(tblk, &ninode->data, 0, sizeof(ninode->data));

	return 0;
}

/*
 * XXX should be validating that the block is a valid inode, etc.
 */
int rpdfs_inode_get(struct rpdfs_fs_info *nfi, struct rpdfs_transaction *txn, nbf_t nbf,
		    struct rpdfs_inode_ino_gen *ig, struct rpdfs_inode_txn_ref *itref)
{
	if (ig->ino == 0) /* probably a buggy caller but could be corruption */
		return -EUCLEAN;

	return rpdfs_txn_get_block(nfi, txn, ig->ino, nbf, &itref->tblk, (void **)&itref->ninode);
}

int rpdfs_inode_alloc(struct rpdfs_fs_info *nfi, struct rpdfs_transaction *txn,
		      struct rpdfs_inode_ino_gen *ig, struct rpdfs_inode_txn_ref *itref)
{
	struct rpdfs_inode_ino_gen new;
	int ret;

	new.gen = 1; /* XXX should look up previous gen and increment */
	ret = rpdfs_txn_alloc_meta(txn, &new.ino) ?:
	      rpdfs_inode_get(nfi, txn, NBF_WRITE | NBF_NEW, &new, itref);

	if (ret == 0)
		*ig = new;

	return ret;
}

/*
 * A transaction that just copies the inode in the block to the caller's
 * buffer.
 */
int rpdfs_inode_read_copy(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *ig,
			  void *buf, int size)
{
	struct rpdfs_inode_txn_ref itref;
	struct rpdfs_transaction txn;
	int ret;

	if (WARN_ON_ONCE(size < 0)) {
		ret = -EINVAL;
		goto out;
	}

	rpdfs_txn_init(&txn);

	do {
		ret = rpdfs_inode_get(nfi, &txn, NBF_READ, ig, &itref);
		if (ret == 0) {
			ret = min(size, sizeof(struct rpdfs_inode));
			memcpy(buf, itref.ninode, ret);
		}
	} while (rpdfs_txn_retry(nfi, &txn, &ret));

	rpdfs_txn_teardown(nfi, &txn);
out:
	return ret;
}

/*
 * Update an inode to reflect the addition or removal of one or more
 * links to it.
 */
int rpdfs_inode_update(struct rpdfs_txn_block *tblk, struct rpdfs_inode *inode, s32 delta)
{
	s32 nlink = le32_to_cpu(inode->nlink);

	if ((delta > 0) && (nlink > RPDFS_LINK_MAX - delta))
		return -EMLINK;

	/* nlink < 0 is a data corruption bug */
	if ((delta < 0) && (nlink + delta < 0))
		return -EUCLEAN;

	/*
	 * If this is the removal of the last external link to a dir, remove the "."
	 * self-link too.
	 */
	if ((nlink == 2 && delta == -1) && ((le32_to_cpu(inode->mode) & S_IFMT) == S_IFDIR))
		delta = -2;

	rpdfs_tblk_assign(tblk, inode->nlink, cpu_to_le32(nlink + delta));

	return 0;
}
