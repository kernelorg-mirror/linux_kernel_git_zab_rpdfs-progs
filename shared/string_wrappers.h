/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_STRING_WRAPPERS_H
#define RPDFS_SHARED_STRING_WRAPPERS_H

#include <string.h>

#define memset_zero_sizeof(x)		memset(&(x), 0, sizeof(x))

/*
 * Move all the elements from the index to the end of the array by the
 * distance in units of the index.  The distance can be negative and the
 * index can be at or past the end of the array.
 */
#define memmove_array_tail(ARR, IND, TOTAL, DIST) \
do { \
	__typeof__(&(ARR)[0]) arr_ = (ARR); \
	__typeof__(IND) ind_ = (IND); \
	__typeof__(TOTAL) total_ = (TOTAL); \
        if (ind_ < total_) \
                memmove(&arr_[ind_ + (DIST)], &arr_[ind_], (total_ - ind_) * sizeof(arr_[0])); \
} while (0)

#endif
