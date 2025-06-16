/* SPDX-License-Identifier: GPL-2.0 */

#include "utask_defs.h"
#include "utask_gen_defs.h"

.text
.align 8

#
# Our called functions that switch stacks look like functions to the
# caller, but jump to the return path from one of our functions in the
# new stack.  That returned-to code can clobber the callee-saved
# registers because it thinks it has saved them in its function prologue
# and epilogue.  So we save and restore them as we switch to and from
# stacks.
#
# ``Registers %rbp, %rbx and %r12 through %r15 “belong” to the calling
# function and the called function is required to preserve their
# values.'' -- x86_64 ABI.
#
.macro push_callee_saved
	push %rbx
	push %r12
	push %r13
	push %r14
	push %r15
	push %rbp
.endm

.macro pop_callee_saved
	pop %rbp
	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %rbx
.endm

#
# set rbx to the address of the utask struct found at the base of the
# stack.
#
.macro load_utask_rbx
	movq %rsp, %rbx
	andq $UTASK_STACK_BASE_MASK, %rbx
	addq $(UTASK_STACK_SIZE - UTASK_ASM_SIZEOF_UTASK), %rbx
.endm

#
# int utask_switch_to(utask *utask /* rdi */);
#
# Resume execution of a utask that was previously stopped by switching
# from the scheduler's stack to the task's.
#
# Save the calling scheduler's stack pointer, restore the utask's stack
# pointer, and return into the address that the utask pushed as they
# last called _switch_from.
#
# The ret here is returning to the caller of _switch_from, which is void,
# so we don't set a return code.
#
.globl utask_switch_to
.type utask_switch_to, @function
utask_switch_to:
	push_callee_saved
	movq %rsp, UTASK_ASM_OFFSETOF_SCHED_SP(%rdi)
	movq UTASK_ASM_OFFSETOF_SP(%rdi), %rsp
	pop_callee_saved
	ret

#
# void utask_switch_from(void)
#
# Pause execution of a utask by switching from the task's stack to the
# scheduler's.
#
# Save the utask's stack, restore the scheduler's, and return into it.
#
# The ret here is returning to the caller of _switch_to and we set the
# code to indicate that the utask scheduled and still running.
#
.globl utask_switch_from
.type utask_switch_from, @function
utask_switch_from:
	push_callee_saved
	load_utask_rbx
	movq %rsp, UTASK_ASM_OFFSETOF_SP(%rbx)
	movq UTASK_ASM_OFFSETOF_SCHED_SP(%rbx), %rsp
	pop_callee_saved
	movq $UTASK_RET_SCHEDULED, %rax
	ret

#
# void utask_finish(void)
#
# Stop execution of a utask and return to the scheduler, but return a
# status that indicates that the utask should be destroyed.
#
# This is called by the _entry that calls the utask fn after it returns,
# we're the highest frame on the stack and have no state.
#
.globl utask_finish
.type utask_finish, @function
utask_finish:
	load_utask_rbx
	movq UTASK_ASM_OFFSETOF_SCHED_SP(%rbx), %rsp
	pop_callee_saved
	mov $UTASK_RET_FINISHED, %rax
	ret
