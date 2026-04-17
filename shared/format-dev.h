/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_FORMAT_DEV_H
#define RPDFS_SHARED_FORMAT_DEV_H

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/limits.h"
#include "shared/lk/types.h"

#include "shared/format-block.h"

/*
 * This describes the structs that are only found in devices.  They're
 * used by devd to manage the blocks that are served over the network.
 */

enum {
	RPDFS_DEV_BLOCK_TYPE_UNINIT = 0,
	RPDFS_DEV_BLOCK_TYPE_COMMIT,
	RPDFS_DEV_BLOCK_TYPE_SUMMARY,
	RPDFS_DEV_BLOCK_TYPE_DETAILS,
	RPDFS_DEV_BLOCK_TYPE_STORED,
	RPDFS_DEV_BLOCK_TYPE__INVALID
};

#define RPDFS_UUID_SIZE	16
struct rpdfs_uuid {
	__u8 bytes[RPDFS_UUID_SIZE];
};

/*
 * Every internal metadata block starts with a header for verification.
 */
struct rpdfs_dev_block_header {
	__le64 crc;
	struct rpdfs_uuid dev_uuid;
	__u8 pad_[7];
	__u8 type;
};

/*
 * Devices are organized into large contiguous regions: a journal,
 * summaries, details, and the stored blocks themselves.
 */
struct rpdfs_dev_layout {
	__le64 commit_blocks;
	__le64 journal_blocks;
	__le64 summary_blocks;
	__le64 details_blocks;
	__le64 storage_blocks;
};

/*
 * Arbitrarily declare a functional minimum number of journal or commit
 * blocks.  The journal blocks are the significant limiting resource,
 * the journal needs to be able to fit a reasonable number of commits.
 */
#define RPDFS_DEV_MIN_JC_BLOCKS 256

/*
 * Each commit block describes all the blocks involved in a coherent
 * atomic change.  The commit block and all its described blocks are
 * written concurrently.  They all need to be checked to verify that the
 * last commit succeeded.
 *
 * We spend some space in every commit block to store the device layout.
 * This saves us from having a separate long-lived format block.
 */
struct rpdfs_dev_commit_block {
	struct rpdfs_dev_block_header hdr;
	struct rpdfs_dev_layout layout;
	__le64 commit_ctr;
	__le64 oldest_commit_ctr;
	__le64 journal_head_ctr;
	__le64 journal_tail_ctr;
	__le16 nr_entries;
	__le16 nr_in_journal;
	__u8 pad_[4];
	struct rpdfs_dev_commit_entry {
		__le64 lba;
		__le64 journ_lba;
		__le64 crc;
		__u8 pad_[7];
		__u8 type;
	} entries[0];
};

#define RPDFS_DEV_COMMIT_MAX_ENTRIES					\
	((RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_dev_commit_block)) /	\
	 sizeof(struct rpdfs_dev_commit_entry))

/*
 * The network protocol operates-on per-block details in addition to its
 * contents.
 *
 * The alloc_ctr is incremented every time the block is allocated or
 * freed.  An allocated block will always have an odd alloc_ctr.
 *
 * The write_ctr is incremented every time the block is written and the
 * contents could have changed.  (The server doesn't pay the read IO
 * cost to check.)
 */
struct rpdfs_block_details {
	__le64 alloc_ctr;
	__le64 write_ctr;
	__le64 place_lo;
	__le64 place_hi;
	__le32 crc;
	__u8 _pad[4];
};

struct rpdfs_dev_details_block {
	struct rpdfs_dev_block_header hdr;
	struct rpdfs_block_details details[0];
};

#define RPDFS_DEV_DETAILS_PER_BLOCK					\
	((RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_dev_details_block)) /	\
	 sizeof(struct rpdfs_block_details))

struct rpdfs_dev_summary_block {
	struct rpdfs_dev_block_header hdr;
	struct rpdfs_dev_summary {
		/*
		 * The total number of entries in the details block that
		 * have an odd alloc_ctr which indicates that they're
		 * allocated and referenced.  Tracks alloc, instead of
		 * free, so that formatting's zeroed summary blocks
		 * correctly summarize zeroed details blocks.
		 */
		u8 alloc_count;
		/*
		 * The total number of entries in the details block that
		 * are allocated and have an inode place type.
		 */
		u8 nr_inodes;
	} summaries[];
};

#define RPDFS_DEV_SUMMARIES_PER_BLOCK					\
	((RPDFS_BLOCK_SIZE - sizeof(struct rpdfs_dev_summary_block)) /	\
	 sizeof(struct rpdfs_dev_summary))

#endif
