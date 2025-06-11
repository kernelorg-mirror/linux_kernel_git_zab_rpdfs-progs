/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_OPTIONS_H
#define NGNFS_SHARED_OPTIONS_H

#include <getopt.h>

/*
 * The longopt options control the general option parsing.  In
 * particular, the "has_arg" constants (no_argument, optional_argument,
 * required_argument) specify if each option has arguments.
 *
 * Our _more flags then add additional constraints.
 */
struct option_more {
	struct option longopt;
	char *arg;
	char *desc;
	unsigned required:1,	/* whether this *option* is required */
		/* remaining internal flags used by parsing, not caller specification */
		 _given:1;
};

typedef int (*opt_parse_t)(int c, char *str, void *arg);

int getopt_long_more(int argc, char *const argv[], struct option_more *moreopts, size_t nr,
		     opt_parse_t, void *arg);

#endif
