// SPDX-License-Identifier: GPL-2.0-or-later

#include "shared/lk/bitops.h"
#include "shared/lk/compiler.h"
#include "shared/lk/minmax.h"

#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))

/*
 * Common helper for find_next_bit() function family
 * @FETCH: The expression that fetches and pre-processes each word of bitmap(s)
 * @MUNGE: The expression that post-processes a word containing found bit (may be empty)
 * @size: The bitmap size in bits
 * @start: The bitnumber to start searching at
 */
#define FIND_NEXT_BIT(FETCH, MUNGE, size, start)                                \
({                                                                              \
        unsigned long mask, idx, tmp, sz = (size), __start = (start);           \
                                                                                \
        if (unlikely(__start >= sz))                                            \
                goto out;                                                       \
                                                                                \
        mask = MUNGE(BITMAP_FIRST_WORD_MASK(__start));                          \
        idx = __start / BITS_PER_LONG;                                          \
                                                                                \
        for (tmp = (FETCH) & mask; !tmp; tmp = (FETCH)) {                       \
                if ((idx + 1) * BITS_PER_LONG >= sz)                            \
                        goto out;                                               \
                idx++;                                                          \
        }                                                                       \
                                                                                \
        sz = min(idx * BITS_PER_LONG + __ffs(MUNGE(tmp)), sz);                  \
out:                                                                            \
        sz;                                                                     \
})

unsigned long find_next_bit(const unsigned long *addr, unsigned long size,
                            unsigned long offset)
{
	return FIND_NEXT_BIT(addr[idx], /* nop */, size, offset);
}
