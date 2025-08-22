/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_LK_MINMAX_H
#define RPDFS_SHARED_LK_MINMAX_H

#define min(a, b)		\
({				\
	__typeof__(a) _a = (a);	\
	__typeof__(b) _b = (b);	\
				\
	_a < _b ? _a : _b;	\
})

#define max(a, b)		\
({				\
	__typeof__(a) _a = (a);	\
	__typeof__(b) _b = (b);	\
				\
	_a > _b ? _a : _b;	\
})

#define swap(a, b)		\
        do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#endif
