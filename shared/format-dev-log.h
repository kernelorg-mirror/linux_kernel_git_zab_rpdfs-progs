/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_FORMAT_DEV_LOG_H
#define RPDFS_SHARED_FORMAT_DEV_LOG_H

#include <linux/types.h>

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/math.h"
#include "shared/lk/types.h"

#include "shared/format-msg.h"

#define RPDFS_LOG_BLOCK_TYPE_COMMIT	0
#define RPDFS_LOG_BLOCK_TYPE_SEARCH	1
#define RPDFS_LOG_BLOCK_TYPE_BTREE	2
#define RPDFS_LOG_BLOCK_TYPE_DATA	3

#define RPDFS_LOG_UUID_SIZE	32

struct rpdfs_log_block_header {
	__u8 uuid[RPDFS_LOG_UUID_SIZE];
	__le64 dev_addr;
	__le64 crc;
	__u8 _pad[7];
	__u8 type;
};

struct rpdfs_log_btree_ref {
	__le64 dev_addr;
};

struct rpdfs_log_btree_root {
	struct rpdfs_log_btree_ref ref;
	__u8 _pad[7];
	__u8 height;
};

struct rpdfs_log_btree_block {
	struct rpdfs_log_block_header hdr;
	__le16 val_size;
	__le16 nr_items;
	__u8 _pad[3];
	__u8 level;
	__le16 offsets[];
};

struct rpdfs_log_btree_key {
	__le64 k[3];
};

struct rpdfs_log_btree_item {
	struct rpdfs_log_btree_key key;
	/* u64 aligned value follows */
};

#define RPDFS_LOG_BTREE_BLOCK_SIZE	4096
#define RPDFS_LOG_BTREE_ALIGNMENT	8

#define RPDFS_LOG_BTREE_FREE_MAX \
	(RPDFS_LOG_BTREE_BLOCK_SIZE - sizeof(struct rpdfs_log_btree_block))

#define RPDFS_LOG_BTREE_EMPTY_ITEM_SIZE \
	 (sizeof_field(struct rpdfs_log_btree_block, offsets[0]) + \
	  sizeof(struct rpdfs_log_btree_item))

/*
 * The max item size is limited so that we have a few items in a half
 * full block.
 */
#define RPDFS_LOG_BTREE_MAX_VAL_SIZE \
	round_down(((RPDFS_LOG_BTREE_FREE_MAX / 8) - RPDFS_LOG_BTREE_EMPTY_ITEM_SIZE), \
		   RPDFS_LOG_BTREE_ALIGNMENT)

/*
 * The commit block comes first in a multi-block write that performs an
 * atomic update of device metadata.  It references the other blocks in
 * the commit that then reference all the existing stable blocks.
 */
struct rpdfs_log_commit_block {
	struct rpdfs_log_block_header hdr;
	struct rpdfs_log_btree_root details_root;
	__le64 total_allocated;
	__le64 total_inodes;
	__le64 commit_seq;
	__u8 _pad[6];
	__le16 nr_entries;
	struct rpdfs_log_commit_entry {
		__le64 crc;
		__u8 _pad[7];
		__u8 type;
	} entries[0];
};

#define RPDFS_LOG_COMMIT_BLOCK_MAX_ENTRIES \
	((RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_log_commit_block)) / \
	 sizeof_field(struct rpdfs_log_commit_block, entries[0]))

struct rpdfs_log_block_details {
	__le64 dev_addr;
	struct rpdfs_msg_block_details det;
};

/*
 * These search blocks only exist to be written at regular offsets and
 * reference commit blocks.
 */
struct rpdfs_log_search_block {
	struct rpdfs_log_block_header hdr;
	__le64 commit_dev_addr;
};

#endif
