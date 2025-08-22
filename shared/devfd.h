/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_DEVFD_H
#define RPDFS_SHARED_DEVFD_H

#include "shared/lk/types.h"

int devfd_get_size(int fd, u64 *size_ret);
int devfd_write_zeros(int fd, u64 start, u64 len);

#endif
