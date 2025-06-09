/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UTASK_H__
#define __UTASK_H__

#include <liburing.h>

#include "shared/lk/bug.h"
#include "shared/lk/list.h"
#include "shared/lk/math.h"
#include "shared/lk/types.h"

#include "utask_defs.h"

#define UTASK_MAGIC ((unsigned long)0x625944a490864a4c)

typedef void (*utask_fn_t)(void *data);

struct utask_wait_queue {
	struct list_head wait_list;
};

#define INIT_UTASK_WAIT_QUEUE(name) \
	{ .wait_list = LIST_HEAD_INIT(name.wait_list) }

struct utask_wait_entry {
	struct list_head queue_head;
	struct utask *tsk;
};

#define INIT_UTASK_WAIT(name) \
	{ .queue_head = LIST_HEAD_INIT(name.queue_head) }
#define DECLARE_UTASK_WAIT(name) \
	struct utask_wait_entry name = INIT_UTASK_WAIT(name);

#define __utask_canceled_err()			\
({						\
	utask_am_canceled() ? -ECANCELED : 0;	\
})

/*
 * Block a utask until the condition is true.  The task is put on the
 * wait queue and it must be woken by other utasks as the condition
 * becomes true.
 *
 * Returns (int)0 if the condition was true and -ECANCELED if the task
 * was canceled and should return from its fn.
 *
 * This naturally expands/evaluates the condition as many times as is
 * needed for wakeups to test the condition and go back to sleep.  Be
 * careful with side-effects of the condition executing.
 *
 * This variant mirror's the kernel's wait_event_*() APIs that are built
 * for multiple tasks on a wait queue all waiting for the condition.
 */
#define utask_wait_event(wq, cond)					\
({									\
	int ret_ = 0;							\
									\
	if (!(cond)) {							\
		DECLARE_UTASK_WAIT(wait_);				\
									\
		utask_prepare_wait((wq), &wait_);			\
		while (!(cond) && !(ret_ = __utask_canceled_err()))	\
			utask_schedule();				\
		utask_finish_wait(&wait_);				\
	}								\
									\
	ret_;								\
})

/*
 * Like _wait_event(), but utasks that change the condition wake the
 * single blocked utask directly instead of waking multiple utasks on a
 * wait queue.
 */
#define utask_wait_event_task(cond)					\
({									\
	int ret_ = 0;							\
									\
	while (!(cond) && !(ret_ = __utask_canceled_err()))		\
		utask_schedule();					\
									\
	ret_;								\
})

/*
 * This waiting variant isn't interrupted by the task being canceled.
 * This should be very rarely used in teardown paths to wait on
 * conditions that are sure to be met.  It lets utasks that are
 * themselves tearing down still make progress while another utask is
 * waiting for them to finish.
 */
#define utask_wait_event_nocancel(cond)					\
do {									\
	while (!(cond))							\
		utask_schedule();					\
} while (0)

/*
 * We make the embedded container type a struct so that we can
 * predeclare it as an argument to the fn.
 */
struct utask_cqe_callback;

typedef void (*utask_cqe_fn_t)(struct io_uring_cqe *cqe, struct utask_cqe_callback *cb);

struct utask_cqe_callback {
	utask_cqe_fn_t fn;
};

struct utask_cqe_waiter {
	struct utask *tsk;
	struct utask_cqe_callback cb;
	s32 res;
	bool completed;
};

extern int utask_switch_to(struct utask *tsk);
extern void utask_schedule(void);
void utask_finish(void);

int utask_run(void);
void utask_shutdown(void);

int utask_create(utask_fn_t fn, void *data, struct utask **tsk_ret);
int utask_create_nowake(utask_fn_t fn, void *data, struct utask **tsk_ret);
struct utask *utask_current(void);
u64 utask_current_id(void);
void utask_cancel(struct utask *tsk);
bool utask_am_canceled(void);
void utask_destroy(struct utask *tsk);
void utask_destroy_other(struct utask *tsk);
void utask_init_wait_queue(struct utask_wait_queue *wq);
bool utask_waitqueue_active(struct utask_wait_queue *wq);
void utask_prepare_wait(struct utask_wait_queue *wq, struct utask_wait_entry *wait);
void utask_finish_wait(struct utask_wait_entry *wait);
void utask_wake_all(struct utask_wait_queue *wq);
void utask_wake_task(struct utask *tsk);

struct io_uring *utask_ring(void);
void utask_set_sqe_callback(struct io_uring_sqe *sqe, struct utask_cqe_callback *cb,
			    utask_cqe_fn_t fn);

struct io_uring_sqe *utask_get_sqe_waiter(struct utask_cqe_waiter *waiter);

int utask_init(u32 entries);
void utask_exit(void);

#endif
