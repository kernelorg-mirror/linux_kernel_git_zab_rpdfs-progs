/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_BLOCK_H
#define RPDFS_SHARED_BLOCK_H

#include <stdlib.h>

struct rpdfs_block;

#include "shared/fs_info.h"
#include "shared/lk/gfp.h"
#include "shared/lk/list.h"
#include "shared/lk/types.h"

typedef enum {
	/*
	 * Acquire a read reference that can be shared with other readers.  Can not
	 * be specified with _WRITE.
	 */
	NBF_READ		= (1 << 0),
	/*
	 * Acquire an exclusive reference which excludes all other
	 * readers and writers.  Can not be specified with _READ.  The
	 * block will be recorded as dirty as this block is put (if
	 * _NODIRTY is not specified with the put.)
	 */
	NBF_WRITE		= (1 << 1),
	/*
	 * Return an error instead of blocking waiting for (read|write) access.
	 */
	NBF_TRY			= (1 << 2),
	/*
	 * The caller is writing and will overwrite the current contents
	 * of the block.  The block will be cached without reading from
	 * the device.  The contents are undefined and the caller is
	 * responsible for initializing the block.
	 */
	NBF_NEW			= (1 << 3),
	/*
	 * Stops a block from being considered dirty as a write
	 * reference is put, if the block wasn't already dirty.  Used as
	 * transactions are unwound and callers have reverted their
	 * modification of the block so that we don't write out blocks
	 * that haven't changed.
	 */
	NBF_NODIRTY		= (1 << 4),
	/*
	 * Must be specified with _WRITE.  The caller is promising, hope
	 * to die, that they already have an existing _READ reference
	 * and would like to convert it to the exclusive write
	 * reference.  This will never block and will return an error if
	 * there were multiple shared read references to the block.
	 */
	NBF_CONVERT_WRITE	= (1 << 5),
} nbf_t;

/* these flags are mutually exclusive */
#define NBF_RW_EXCL	(NBF_READ | NBF_WRITE)

struct rpdfs_block *rpdfs_block_get(struct rpdfs_fs_info *nfi, u64 bnr, nbf_t nbf);
void rpdfs_block_put(struct rpdfs_fs_info *nfi, struct rpdfs_block *bl, nbf_t nbf);
void *rpdfs_block_buf(struct rpdfs_block *bl);
struct page *rpdfs_block_page(struct rpdfs_block *bl);
void rpdfs_block_dirty_limit_wait(struct rpdfs_fs_info *nfi);

int rpdfs_block_dirty_begin(struct rpdfs_fs_info *nfi, struct list_head *list, ssize_t off);
void rpdfs_block_dirty_end(struct rpdfs_fs_info *nfi, struct list_head *list, ssize_t off);
int rpdfs_block_sync(struct rpdfs_fs_info *nfi);

int rpdfs_block_setup(struct rpdfs_fs_info *nfi, int queue_depth);
void rpdfs_block_destroy(struct rpdfs_fs_info *nfi);

#endif

