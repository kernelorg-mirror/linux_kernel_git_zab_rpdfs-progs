/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_VALGRIND_SUPPORT_H
#define RPDFS_SHARED_VALGRIND_SUPPORT_H

#include <string.h>
#include <valgrind/valgrind.h>

/*
 * We do these unconditionally today but we put them all in one place so
 * it'd be easy to flip it all off with a bit of build tooling.
 */

#define VGS_DEFINE_STACK_ID(name) u64 name

#define VGS_STACK_REGISTER(id, start, end)		\
do {							\
	id = VALGRIND_STACK_REGISTER(start, end);	\
} while (0)

#define VGS_STACK_DEREGISTER(id)			\
	VALGRIND_STACK_DEREGISTER(id)			\

/*
 * Mark a buffer as initialized if we're running in valgrind.  This is
 * used for buffers that are set by mechanisms that valgrind doesn't
 * see.  At the time of this writing, it's io_uring read commands.  I
 * don't think there's client requests to mark buffers as initialized so
 * we just memset them :/.
 */
static inline void VGS_INIT_BUF(void *ptr, size_t len)
{
	if (RUNNING_ON_VALGRIND)
		memset((ptr), 0, (len));
}

#endif
