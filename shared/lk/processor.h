/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_PROCESSOR_H
#define NGNFS_SHARED_LK_PROCESSOR_H

#include "shared/urcu.h"

static inline void cpu_relax(void)
{
	caa_cpu_relax();
}

#endif
