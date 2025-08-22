/* SPDX-License-Identifier: GPL-2.0 OR MIT */
#ifndef __LINUX_OVERFLOW_H
#define __LINUX_OVERFLOW_H

#include <stdbool.h>

#include "shared/lk/compiler.h"
#include "shared/lk/compiler_attributes.h"

/*
 * Allows for effectively applying __must_check to a macro so we can have
 * both the type-agnostic benefits of the macros while also being able to
 * enforce that the return value is, in fact, checked.
 */
static inline bool __must_check __must_check_overflow(bool overflow)
{
	return unlikely(overflow);
}

/**
 * check_add_overflow() - Calculate addition with overflow checking
 * @a: first addend
 * @b: second addend
 * @d: pointer to store sum
 *
 * Returns true on wrap-around, false otherwise.
 *
 * *@d holds the results of the attempted addition, regardless of whether
 * wrap-around occurred.
 */
#define check_add_overflow(a, b, d)	\
	__must_check_overflow(__builtin_add_overflow(a, b, d))

/**
 * check_sub_overflow() - Calculate subtraction with overflow checking
 * @a: minuend; value to subtract from
 * @b: subtrahend; value to subtract from @a
 * @d: pointer to store difference
 *
 * Returns true on wrap-around, false otherwise.
 *
 * *@d holds the results of the attempted subtraction, regardless of whether
 * wrap-around occurred.
 */
#define check_sub_overflow(a, b, d)	\
	__must_check_overflow(__builtin_sub_overflow(a, b, d))

/**
 * check_mul_overflow() - Calculate multiplication with overflow checking
 * @a: first factor
 * @b: second factor
 * @d: pointer to store product
 *
 * Returns true on wrap-around, false otherwise.
 *
 * *@d holds the results of the attempted multiplication, regardless of whether
 * wrap-around occurred.
 */
#define check_mul_overflow(a, b, d)	\
	__must_check_overflow(__builtin_mul_overflow(a, b, d))

#endif /* __LINUX_OVERFLOW_H */
