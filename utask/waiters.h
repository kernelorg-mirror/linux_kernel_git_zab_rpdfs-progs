/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UTASK_WAITERS_H__
#define __UTASK_WAITERS_H__

#include "utask/utask.h"

int utask_read(int fd, void *buf, unsigned count, u64 offset);
int utask_writev(int fd, struct iovec *iovecs, unsigned nr_vecs, u64 offset, u64 total);

#endif
