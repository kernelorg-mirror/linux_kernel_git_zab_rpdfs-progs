/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UTASK_ASM_H__
#define __UTASK_ASM_H__

/*
 * This is included by both C and asm source.
 */

/* XXX lots of x86/64bit assumptions */

#define UTASK_STACK_SIZE	(8 * 1024)
#define UTASK_STACK_BASE_MASK	(~(UTASK_STACK_SIZE - 1))

#define UTASK_SAVED_BYTES	(6 * 8)

#define UTASK_RET_SCHEDULED	0
#define UTASK_RET_FINISHED	1

#endif
