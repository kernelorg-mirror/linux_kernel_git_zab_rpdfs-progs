/* SPDX-License-Identifier: GPL-2.0 */

#include "utask/utask.h"
#include "utask/waiters.h"

/*
 * This contains wrappers for io_uring_prep_*() commands.  The task
 * registers a callback on its stack and sleeps until the completion
 * wakes it.  All the wrappers return 0/-errno.
 */

int utask_read(int fd, void *buf, unsigned count, u64 offset)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_read(sqe, fd, buf, count, offset);
	ret = utask_wait_event_task(waiter.completed);
	if (ret == 0) {
		ret = waiter.res;
		if (ret >= 0) {
			if (ret != count)
				ret = -EIO;
			else
				ret = 0;
		}
	}

	return ret;
}

int utask_writev(int fd, struct iovec *iovecs, unsigned nr_vecs, u64 offset, u64 total)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_writev(sqe, fd, iovecs, nr_vecs, offset);
	ret = utask_wait_event_task(waiter.completed);
	if (ret == 0) {
		ret = waiter.res;
		if (ret >= 0) {
			if (ret != total)
				ret = -EIO;
			else
				ret = 0;
		}
	}

	return ret;
}
