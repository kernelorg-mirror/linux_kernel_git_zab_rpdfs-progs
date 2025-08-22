/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_DIR_H
#define RPDFS_SHARED_DIR_H

#include "shared/fs_info.h"
#include "shared/inode.h"

int rpdfs_dir_create(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *dir, umode_t mode,
		     char *name, size_t name_len);
int rpdfs_dir_mkdir(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *dir, umode_t mode,
		    char *name, size_t name_len);
int rpdfs_dir_unlink(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *dir, char *name,
		     size_t name_len);
int rpdfs_dir_rmdir(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *dir, char *name,
		    size_t name_len);
int rpdfs_dir_rename(struct rpdfs_fs_info *nfi,
		     struct rpdfs_inode_ino_gen *src_dir_ig, char *src_name, size_t src_name_len,
		     struct rpdfs_inode_ino_gen *dst_dir_ig, char *dst_name, size_t dst_name_len);

/*
 * Readdir fills the buffer with entries.  The start of the buffer must
 * be at least as aligned as the entry.  Entries are padded for
 * alignment.  Use next_off to iterate through them instead of trying to
 * skip name length bytes past the end of the struct.
 */
struct rpdfs_readdir_entry {
	u64 pos;
	struct rpdfs_inode_ino_gen ig;
	u16 next_offset;		/* bytes to add to entry to get next entry */
	u8 dtype;
	u8 name_len;			/* does not include terminating null, like strlen */
	u8 name[];			/* is null terminated, name_len doesn't include null byte */
};

/*
 * Minimum readdir entry buf size needed to store a single entry with a
 * maximum name size.  The buffer must be this size or an error is
 * returned.
 */
#define RPDFS_READDIR_MIN_BUF_SIZE \
	offsetof(struct rpdfs_readdir_entry , name[RPDFS_NAME_MAX + 1])

/*
 * Returns the number of entries filled into the buffer, not the number
 * of bytes.
 */
int rpdfs_dir_readdir(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *dir_ig, u64 pos,
		      struct rpdfs_readdir_entry *buf, size_t size);

/*
 * Lookup returns only the inode number, generation, and the POSIX ABI
 * file type.
 */
struct rpdfs_dir_lookup_entry {
	struct rpdfs_inode_ino_gen ig;
	u8 dtype;
};

int rpdfs_dir_lookup(struct rpdfs_fs_info *nfi, struct rpdfs_inode_ino_gen *dir_ig, char *name,
		     size_t name_len, struct rpdfs_dir_lookup_entry *lent);

#endif
