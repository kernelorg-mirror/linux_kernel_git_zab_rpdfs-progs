/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_DTRACEF_H
#define RPDFS_SHARED_DTRACEF_H

#include <unistd.h>
#include <stdio.h>

#include "shared/lk/build_bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/limits.h"
#include "shared/lk/list.h"
#include "shared/lk/types.h"

#include "shared/format-dtracef.h"
#include "shared/va.h"

extern bool dtracef_enabled;

struct dtracef_site {
	struct list_head head;
	char *name;
	char *fmt;
	char *file;
	unsigned int line;
	u16 nr;
	u8 nr_args;
};

__attribute__ ((format(printf, 1, 2)))
static inline void __check_printf_args(char *fmt, ...)
{
}

/*
 * the trailing ';' is intentional, VA_FOR_EACH_N doesn't add separators.
 */
#define __store_args(a, b) \
	do { ev_->args[b] = cpu_to_le64((u64)(a)); } while (0);

/*
 * Register each site by having a nested function that's marked as a
 * constructor instead of trying to put all the sites in an elf section
 * and iterating over them.
 */
#define __register_site(SITE_)						\
do {									\
	void __attribute__((constructor)) __regster_site_ctor(void) {	\
		dtracef_register_site(SITE_);				\
	}								\
} while (0)

/*
 * Record a printf formatted trace event.
 *
 * This has limitations in what it supports:
 *  - format must not end in either nl/cr
 *  - must have arguments
 *  - arguments can't be strings, wide characters, or 128bit integers
 *  - no format specifiers with multiple arguments (%*)
 *  - (XXX casting floats today, not great)
 */
#define dtracef(NAME_, FMT_, ...)						\
do {										\
	static struct dtracef_site site_ = {					\
		.name = NAME_,							\
		.fmt = FMT_,							\
		.file = __FILE__,						\
		.line = __LINE__,						\
		.nr_args = VA_NR_ARGS(__VA_ARGS__),				\
	};									\
										\
	__register_site(&site_);						\
	__check_printf_args(FMT_, __VA_ARGS__);					\
										\
	if (dtracef_enabled) {							\
		struct rpdfs_dtracef_event *ev_ = dtracef_get_event(&site_);	\
		VA_FOR_EACH_N(__store_args, __VA_ARGS__);			\
	}									\
} while (0)

struct rpdfs_dtracef_event *dtracef_get_event(struct dtracef_site *site);
void dtracef_register_site(struct dtracef_site *site);
int dtracef_print_trace_mem(void *addr, size_t size);

int dtracef_init(char *path);
void dtracef_exit(void);

#endif
