/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* sigdescr_np, sigabbrev_np */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include <shared/lk/kernel.h>
#include "shared/crash.h"

/*
 * Try and print as much helpful info as possible to stderr to help in
 * triage without requiring tracking down cores.  oops, bsod, etc.
 */

static void *bt_buffer[1000];

/*
 * backtrace_symbols_fd is easy because it is available but we'll want
 * to move to libbacktrace.
 *
 * This is relying on providing -rdynamic to the gnu linker.
 */
static void crash_handler(int sig, siginfo_t *si, void *ucontext)
{
	const int fd = STDERR_FILENO;
	int nr;

	dprintf(fd, "[CRASH: caught signal: %s (%s, %d), info addr %p, backtrace:]\n",
		sigdescr_np(sig), sigabbrev_np(sig), sig, si->si_addr);

	nr = backtrace(bt_buffer, ARRAY_SIZE(bt_buffer));
	backtrace_symbols_fd(bt_buffer, nr, fd);
	dprintf(fd, "[CRASH: raising signal with default handler]\n");

	/* reset to the default handler and raise signal to still generate cores */
	signal(sig, SIG_DFL);
	kill(getpid(), sig);
}

int crash_init(void)
{
	struct sigaction sa = {
		.sa_sigaction = crash_handler,
		.sa_flags = SA_SIGINFO,
	};

	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);

	/* backtrace_symbols_fd can load libgdb, ensure loading with dummy call */
	backtrace(bt_buffer, ARRAY_SIZE(bt_buffer));

	return 0;
}
