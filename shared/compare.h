/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_COMPARE_H
#define RPDFS_SHARED_COMPARE_H

/*
 * A type-agnostic comparison that returns -1, 0, 1 ints for <, ==, and
 * > respectively.
 */
#define rpdfs_compare(a, b)				\
({							\
	__typeof__(a) a_ = (a);				\
	__typeof__(b) b_ = (b);				\
							\
	(int)(a_ < b_ ? -1 : a_ > b_ ? 1 : 0);		\
})

#endif
