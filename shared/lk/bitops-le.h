/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_LK_BITOPTS_LE_H
#define RPDFS_SHARED_LK_BITOPTS_LE_H

/*
 * This dropped const and __ variants.
 */

#include "shared/lk/types.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/bitops.h"

#if defined(__LITTLE_ENDIAN)

#define BITOP_LE_SWIZZLE	0

#elif defined(__BIG_ENDIAN)

#define BITOP_LE_SWIZZLE	((BITS_PER_LONG-1) & ~0x7)

#endif


static inline int test_bit_le(int nr, void *addr)
{
	return test_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

static inline void set_bit_le(int nr, void *addr)
{
	set_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

static inline void clear_bit_le(int nr, void *addr)
{
	clear_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

static inline int test_and_set_bit_le(int nr, void *addr)
{
	return test_and_set_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

static inline int test_and_clear_bit_le(int nr, void *addr)
{
	return test_and_clear_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

#endif /* RPDFS_SHARED_LK_BITOPTS_LE_H */
