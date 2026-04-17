/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DETAILS_H
#define RPDFS_DETAILS_H

static inline bool rpdfs_alloc_ctr_is_free(u64 alloc_ctr)
{
	return (alloc_ctr & 1) == 0;
}

#endif
