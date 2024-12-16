/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_COMPARE_H
#define NGNFS_SHARED_COMPARE_H

/*
 * A type-agnostic comparison that returns -1, 0, 1 ints for <, ==, and
 * > respectively.
 */
#define ngnfs_compare(a, b)				\
({							\
	__typeof__(a) a_ = (a);				\
	__typeof__(b) b_ = (b);				\
							\
	(int)(a_ < b_ ? -1 : a_ > b_ ? 1 : 0);		\
})

#endif
