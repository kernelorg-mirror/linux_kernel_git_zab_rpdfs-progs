/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DEVD_FREE_MAP_H
#define RPDFS_DEVD_FREE_MAP_H

void free_map_set_free(u64 bnr, u8 free);
void free_map_add_cached(u64 bnr, s8 delta);
int free_map_find_first_most(u64 *bnr_ret);

int free_map_init(u64 blocks, u64 stripe_size, u64 nr_stripes, u64 my_stripe);
void free_map_exit(void);

#endif
