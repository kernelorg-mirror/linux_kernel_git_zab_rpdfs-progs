/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_INODE_H
#define RPDFS_SHARED_INODE_H

#include "shared/block.h"
#include "shared/fs_info.h"
#include "shared/txn.h"

/*
 * Transactions work with references to inode struct in blocks.
 */
struct rpdfs_inode_txn_ref {
	struct rpdfs_txn_block *tblk;
	struct rpdfs_inode *ninode;
};

struct rpdfs_inode_ino_gen {
	u64 ino;
	u64 gen;
};

/*
 * Make it easy to compare inode number/generation number in any format.
 */

#define _ig_ino(a) \
        _Generic(a, struct rpdfs_ino_gen *: le64_to_cpu((a)->ino),	\
		 struct rpdfs_inode_ino_gen *: (a)->ino)
#define _ig_gen(a) \
        _Generic(a, struct rpdfs_ino_gen *: le64_to_cpu((a)->gen),	\
		 struct rpdfs_inode_ino_gen *: (a)->gen)

#define igs_equal(a, b) \
        (_ig_ino(a) == _ig_ino(b) && _ig_gen(a) == _ig_gen(b))

int rpdfs_inode_init(struct rpdfs_inode_txn_ref *itref, struct rpdfs_inode_ino_gen *ig, u32 nlink,
		     umode_t mode, u64 nsec, struct rpdfs_inode_ino_gen *parent_ig);
int rpdfs_inode_get(struct rpdfs_fs_info *nfi, struct rpdfs_transaction *txn, nbf_t nbf,
		    struct rpdfs_inode_ino_gen *ig, struct rpdfs_inode_txn_ref *itref);
int rpdfs_inode_alloc(struct rpdfs_fs_info *nfi, struct rpdfs_transaction *txn,
		      struct rpdfs_inode_ino_gen *ig, struct rpdfs_inode_txn_ref *itref);
int rpdfs_inode_read_copy(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *ig,
			  void *buf, int size);
int rpdfs_inode_update(struct rpdfs_txn_block *tblk, struct rpdfs_inode *inode, s32 delta);

#endif
