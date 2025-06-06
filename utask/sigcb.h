/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UTASK_SIGCB_H__
#define __UTASK_SIGCB_H__

#include <signal.h>

struct signal_callback;
typedef void (*signal_callback_fn_t)(siginfo_t *si, void *arg);

int sigcb_register_callback(const sigset_t *mask, signal_callback_fn_t fn, void *arg,
			    struct signal_callback **sigcb_ret);
void sigcb_free(struct signal_callback *sigcb);

#endif
