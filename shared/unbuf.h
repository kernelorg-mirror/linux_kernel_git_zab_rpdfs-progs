/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_UNBUF_H
#define RPDFS_SHARED_UNBUF_H

struct rpdfs_undo_buf;

int rpdfs_unbuf_alloc(void *base, size_t bytes, struct rpdfs_undo_buf **unbuf_ret);
void rpdfs_unbuf_free(struct rpdfs_undo_buf *unbuf);

void rpdfs_unbuf_save(struct rpdfs_undo_buf *unbuf, void *ptr, size_t size);
void rpdfs_unbuf_restore(struct rpdfs_undo_buf *unbuf);

#endif
