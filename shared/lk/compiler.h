/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_LK_COMPILER_H
#define RPDFS_SHARED_LK_COMPILER_H

#include "shared/urcu.h"

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define barrier(x)	cmm_barrier()

#endif
