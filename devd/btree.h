/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DEVD_BTREE_H
#define RPDFS_DEVD_BTREE_H

#include "shared/format-dev-log.h"

#include "utask/blk.h"

int btree_prepare_insert(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
			 u16 val_size, struct blk_ticket *tkt);
int btree_prepare_delete(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
			 struct blk_ticket *tkt);

int btree_insert(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
		 void *val, u16 val_size);
int btree_replace(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
		  void *val, void *old, u16 val_size);
int btree_delete(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key);
int btree_lookup(struct rpdfs_log_btree_root *root, struct rpdfs_log_btree_key *key,
		 void *val, u16 val_size, struct blk_ticket *tkt);

#endif
