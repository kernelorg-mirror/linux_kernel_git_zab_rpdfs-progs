/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_BLOCK_H
#define NGNFS_SHARED_FORMAT_BLOCK_H

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/limits.h"
#include "shared/lk/types.h"

#define NGNFS_BLOCK_SHIFT	12
#define NGNFS_BLOCK_SIZE	(1 << NGNFS_BLOCK_SHIFT)

struct ngnfs_block_ref {
	__le64 bnr;
	__le64 alloc_counter; /* XXX */
};

/*
 * The height is one greater than the level of the referenced block.
 * It's 0 for an empty tree.
 */
struct ngnfs_btree_root {
	struct ngnfs_block_ref ref;
	__u8 _pad[7];
	__u8 height;
};

/*
 * Keys are relatively large to allow precise deletion of index keys
 * which contain the generated 64bit key material as well as the logical
 * identity of the inode that generated the key.  The words in the key
 * value array are stored from most to least significant (k[0] is most
 * significant).
 */
struct ngnfs_btree_key {
	__le64 k[3];
};

/*
 * The first and last keys record the range of item keys that can be
 * found in the block.  It's dependent on (and redundant with) the keys
 * of separating parent items.  Having it in the block makes tracking
 * the range a bit easier to use and to update as we merge and split.
 */
struct ngnfs_btree_block {
	struct ngnfs_btree_key first;
	struct ngnfs_btree_key last;
	__le64 bnr;
	__le16 nr_items;
	__le16 tail_free;
	__le16 total_free;
	__u8 level;
	__u8 _pad[1];
	struct ngnfs_btree_item_header {
		__le16 off;
		__le16 val_size;
	} ihdrs[];
};

/*
 * The item's value payload is 64bit aligned and immediately follows the
 * item struct in the block.
 */
struct ngnfs_btree_item {
	struct ngnfs_btree_key key;
	__u8 val[];
};

/*
 * We want to avoid there only being a few items in a full block so we
 * chose a reasonably small fraction of the block size.  The array of
 * item headers is sized to fit the max number of items with no value
 * payload, while aligning the first item after the array.
 */
#define NGNFS_BTREE_MAX_VAL_SIZE	511
#define NGNFS_BTREE_ITEM_ALIGN		8
#define NGNFS_BTREE_MAX_ITEMS									\
	ALIGN_DOWN(((NGNFS_BLOCK_SIZE - sizeof(struct ngnfs_btree_block)) /			\
		   (sizeof(struct ngnfs_btree_key) + sizeof(struct ngnfs_btree_item_header))),	\
		   NGNFS_BTREE_ITEM_ALIGN)
#define NGNFS_BTREE_MAX_FREE									\
	(NGNFS_BLOCK_SIZE - offsetof(struct ngnfs_btree_block, ihdrs[NGNFS_BTREE_MAX_ITEMS]))

/*
 * Inodes are stored in inode blocks.  Inode blocks numbers are directly
 * calculated from the inode number.  The block itself is formatted as a
 * btree block and the inodes (and other inline inode data) are stored
 * as btree items in the block.
 */
struct ngnfs_inode {
	__le64 ino;
	__le64 gen;
	__le64 size;
	__le64 version;
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le32 mode;
	__le32 rdev;
	__le32 flags;
	__le64 atime_nsec;
	__le64 ctime_nsec;
	__le64 mtime_nsec;
	__le64 crtime_nsec;
};

#define NGNFS_ROOT_INO 1

#endif
