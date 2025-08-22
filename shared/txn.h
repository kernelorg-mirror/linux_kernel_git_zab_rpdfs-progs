/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_TXN_H
#define RPDFS_SHARED_TXN_H

struct rpdfs_transaction;

#include "shared/lk/byteorder.h"
#include "shared/lk/list.h"
#include "shared/lk/rbtree.h"

#include "shared/block.h"
#include "shared/unbuf.h"

struct rpdfs_txn_block;

#define rpdfs_tblk_assign(tblk, lhs, rhs)			\
do {								\
	__typeof__(lhs) *lhs_ = &(lhs);				\
								\
	rpdfs_txn_save(tblk, lhs_, sizeof(*lhs_));		\
	*lhs_ = (rhs);						\
} while (0)							\

#define rpdfs_tblk_memcpy(tblk, dst, src, n)			\
do {								\
	__typeof__(dst) dst_ = (dst);				\
	__typeof__(n) n_ = (n);					\
								\
	rpdfs_txn_save(tblk, dst_, n_);				\
	memcpy(dst_, (src), n_);				\
} while (0)

#define rpdfs_tblk_memset(tblk, s, c, n)			\
do {								\
	__typeof__(s) s_ = (s);					\
	__typeof__(n) n_ = (n);					\
								\
	rpdfs_txn_save(tblk, s_, n_);				\
	memset(s_, (c), n_);					\
} while (0)

#define rpdfs_tblk_memmove(tblk, dst, src, n)			\
do {								\
	__typeof__(dst) dst_ = (dst);				\
	__typeof__(n) n_ = (n);					\
								\
	rpdfs_txn_save(tblk, dst_, n_);				\
	memmove(dst_, (src), n_);				\
} while (0)

#define rpdfs_tblk_le16_add_cpu(tblk, lep, val)			\
do {								\
	__typeof__(lep) lep_ = (lep);				\
								\
	rpdfs_txn_save(tblk, lep_, sizeof(*lep_));		\
	le16_add_cpu(lep_, (val));				\
} while (0)

/*
 * Given a (buf) buffer of (total) size, zero the tail bytes
 * after (head) initial bytes, skipping (head) initial bytes.
 */
#define rpdfs_tblk_zero_tail(tblk, buf, head, total)		\
do {								\
	__typeof__(head) head_ = (head);			\
	__typeof__(total) tail_ = (total) - head_;		\
								\
	BUG_ON(head > total);					\
	rpdfs_tblk_memset((tblk), (void *)(buf) + head_, 0, tail_);	\
} while (0)

/*
 * We expose the type so callers can allocate and initialize it, but they don't
 * use it directly.
 */
struct rpdfs_transaction {
	struct rb_root blocks;
	struct list_head writes;
	bool retry_triggered;
};

#define INIT_RPDFS_TXN(txn) {				\
	.blocks = RB_ROOT,				\
	.writes = LIST_HEAD_INIT(txn.writes),		\
	.retry_triggered = false,			\
}

static inline void rpdfs_txn_init(struct rpdfs_transaction *txn)
{
	txn->blocks = RB_ROOT;
	INIT_LIST_HEAD(&txn->writes);
	txn->retry_triggered = false;
}

int rpdfs_txn_get_block(struct rpdfs_fs_info *nfi, struct rpdfs_transaction *txn,
			u64 bnr, nbf_t nbf, struct rpdfs_txn_block **tblk_ret, void **data_ret);
int rpdfs_txn_alloc_meta(struct rpdfs_transaction *txn, u64 *bnr_ret);
void rpdfs_txn_save(struct rpdfs_txn_block *tblk, void *ptr, size_t size);
bool rpdfs_txn_retry(struct rpdfs_fs_info *nfi, struct rpdfs_transaction *txn, int *ret);
void rpdfs_txn_teardown(struct rpdfs_fs_info *nfi, struct rpdfs_transaction *txn);

#endif
