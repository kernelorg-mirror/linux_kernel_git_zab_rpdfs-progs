/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_STRING_H
#define NGNFS_SHARED_LK_STRING_H

#include <string.h>

static inline bool mem_is_zero(const void *s, size_t n)
{
	const unsigned char *buf = s;

	/* unaligned, but cute! */
	return n == 0 || (buf[0] == 0 && (n == 1 || !memcmp(buf, buf + 1, n - 1)));
}

#endif
