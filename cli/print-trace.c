/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "shared/dtracef.h"
#include "shared/format-dtracef.h"
#include "shared/log.h"

#include "cli/cli.h"

static int print_trace_func(int argc, char **argv)
{
	void *addr = MAP_FAILED;
	struct stat st;
	int fd = -1;
	int ret;

	/*
	 * XXX how are we doing option parsing in commands?
	 */
	if (argc != 2) {
		printf("incorrect argc %d\n", argc);
		ret = -EINVAL;
		goto out;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		printf("error opening '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	ret = fstat(fd, &st);
	if (ret < 0) {
		ret = -errno;
		printf("fstat error: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	addr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		ret = -errno;
		goto out;
	}

	ret = dtracef_print_trace_mem(addr, st.st_size);
out:
	if (addr && addr != MAP_FAILED)
		munmap(addr, st.st_size);
	if (fd >= 0)
		close(fd);

	if (ret < 0)
		printf("error: %s (%d)\n", strerror(-ret), -ret);

	return ret;
}

static struct cli_command print_trace_cmd = {
	.func = print_trace_func,
	.name = "print-trace",
	.desc = "print-trace desc",
};

CLI_REGISTER(print_trace_cmd);
