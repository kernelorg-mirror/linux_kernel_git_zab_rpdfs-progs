/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DEVD_CACHE_MODE_H
#define RPDFS_DEVD_CACHE_MODE_H

int cache_mode_request(struct sockaddr_in *addr, u64 bnr, u8 mode, bool is_read, bool with_data);
int cache_mode_confirm(struct sockaddr_in *addr, u64 bnr, u8 mode);

int cache_mode_grant_bulk_uncached(struct sockaddr_in *addr, int mode, u64 bmap_bnr,
				   unsigned long *bmap, size_t size);
void cache_mode_undo_bulk_grant(struct sockaddr_in *addr, u64 bnr,
				unsigned long *bmap, size_t size);
void cache_mode_accessed(struct sockaddr_in *addr, u64 bnr);

int cache_mode_init(void);
void cache_mode_exit(void);

#endif
