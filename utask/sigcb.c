/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* sigorset */
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/signalfd.h>

#include "shared/valgrind_support.h"

#include "utask/sigcb.h"
#include "utask/utask.h"

struct signal_callback {
	int fd;
	sigset_t oldset;
	struct utask *read_tsk;
	signal_callback_fn_t fn;
	void *arg;
};

static int read_waiter(int fd, void *buf, unsigned nbytes, __u64 offset)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_read(sqe, fd, buf, nbytes, offset);
	ret = utask_wait_event_task(waiter.completed);
	if (ret == 0)
		ret = waiter.res;

	return ret;
}

static void signalfd_read_utask(void *data)
{
	struct signal_callback *sigcb = data;
	struct signalfd_siginfo ssi;
	siginfo_t si;
	ssize_t ret;

	VGS_INIT_BUF(&ssi, sizeof(ssi));

	while (!utask_am_canceled()) {

		ret = read_waiter(sigcb->fd, &ssi, sizeof(ssi), 0);
		if (ret != sizeof(ssi)) {
			/* freeing */
			if (ret == -EBADF || ret == -ECANCELED)
				break;
			/* XXX we need some kind of panicf()? */
			printf("signalfd read error: %zd\n", ret);
			exit(1);
		}

		/* not all si fields are supported by ssi */
		memset(&si, 0, sizeof(si));
		si.si_signo = ssi.ssi_signo;
		si.si_errno = ssi.ssi_errno;
		si.si_code = ssi.ssi_code;
		si.si_pid = ssi.ssi_pid;
		si.si_uid = ssi.ssi_uid;
		si.si_status = ssi.ssi_status;
		si.si_utime = ssi.ssi_utime;
		si.si_stime = ssi.ssi_stime;
		si.si_int = ssi.ssi_int;
		si.si_ptr = (void *)(intptr_t)ssi.ssi_ptr;
		si.si_overrun = ssi.ssi_overrun;
		si.si_timerid = ssi.ssi_tid;
		si.si_addr = (void *)(intptr_t)ssi.ssi_addr;
		si.si_band = ssi.ssi_band;
		si.si_fd = ssi.ssi_fd;
		si.si_addr_lsb = ssi.ssi_addr_lsb;

		sigcb->fn(&si, sigcb->arg);
	}
}

/*
 * Register a callback that will be called within a utask when signals
 * are delivered.
 *
 * This sets up all the glue to have a utask blocked reading signalfd
 * info each time any of the callers signals are delivered.
 *
 * The callback will be called in a utask associated with the
 * registration and the callback can be called multiple times.
 *
 * The specified signals will be blocked so that they are only delivered
 * via signalfd.  The signals are unblocked when the registration is
 * freed.
 */
int sigcb_register_callback(const sigset_t *mask, signal_callback_fn_t fn, void *arg,
			    struct signal_callback **sigcb_ret)
{
	struct signal_callback *sigcb = NULL;
	sigset_t set;
	int fd = -1;
	int ret;

	sigcb = malloc(sizeof(struct signal_callback));
	if (!sigcb) {
		ret = -errno;
		goto out;
	}
	sigemptyset(&sigcb->oldset);

	ret = sigprocmask(SIG_SETMASK, NULL, &sigcb->oldset);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	sigorset(&set, &sigcb->oldset, mask);
	ret = sigprocmask(SIG_SETMASK, &set, NULL);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	ret = signalfd(-1, mask, 0);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}
	fd = ret;

	ret = utask_create(signalfd_read_utask, sigcb, &sigcb->read_tsk);
	if (ret < 0)
		goto out;

	sigcb->fd = fd;
	sigcb->fn = fn;
	sigcb->arg = arg;
	ret = 0;
out:
	if (ret < 0) {
		if (sigcb && !sigisemptyset(&sigcb->oldset))
			sigprocmask(SIG_SETMASK, &sigcb->oldset, NULL);
		if (fd >= 0)
			close(fd);
		free(sigcb);
		sigcb = NULL;
	}

	*sigcb_ret = sigcb;
	return ret;
}

void sigcb_free(struct signal_callback *sigcb)
{
	if (sigcb) {
		close(sigcb->fd);
		utask_destroy(sigcb->read_tsk);
		sigprocmask(SIG_SETMASK, &sigcb->oldset, NULL);
		free(sigcb);
	}
}
