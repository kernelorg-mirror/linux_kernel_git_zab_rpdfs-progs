/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_LK_BUILD_BUG_H
#define RPDFS_SHARED_LK_BUILD_BUG_H

#include <assert.h>

#define BUILD_BUG_ON(cond) \
        static_assert(!(cond), "!(" #cond ")")

#endif
