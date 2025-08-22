/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_LK_XXHASH_H
#define RPDFS_SHARED_LK_XXHASH_H

#include <xxhash.h>

static inline uint64_t xxh64(const void *input, size_t length, uint64_t seed)
{
	return XXH64(input, length, seed);
}

#endif

