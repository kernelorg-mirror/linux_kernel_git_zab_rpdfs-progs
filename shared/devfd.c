/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* fallocate() */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/ioctl.h>

#include "shared/lk/types.h"

#include "shared/devfd.h"
#include "shared/nerr.h"

/*
 * Some functions for working with devices as file descriptors in
 * userspace.  We need slightly different behavior when typically
 * working with block devices or when less frequently working with
 * regular files.
 */

int devfd_get_size(int fd, u64 *size_ret)
{
	struct stat st;
	u64 size = 0;
	int ret;

	ret = fstat_nerr(fd, &st);
	if (ret < 0)
		goto out;

	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &size)) {
			ret = -errno;
			goto out;
		}

	} else if (S_ISREG(st.st_mode)) {
		size = st.st_size;
	} else {
		ret = -EINVAL;
		goto out;
	}

	ret = 0;
out:
	*size_ret = size;
	return ret;
}

static int range_command(int fd, int blk, int fal, u64 start, u64 len)
{
	struct stat st;
	u64 range[2];
	int ret;

	ret = fstat_nerr(fd, &st);
	if (ret < 0)
		goto out;

	if (S_ISBLK(st.st_mode)) {
		range[0] = start;
		range[1] = len;

		if (ioctl(fd, blk, range)) {
			ret = -errno;
			goto out;
		}

	} else if (S_ISREG(st.st_mode)) {
		ret = fallocate(fd, fal, start, len);
		if (ret == 0)
			ret = fsync(fd);
		if (ret < 0) {
			ret = -errno;
			goto out;
		}
	} else {
		ret = -EINVAL;
		goto out;
	}

	ret = 0;
out:
	return ret;
}

int devfd_write_zeros(int fd, u64 start, u64 len)
{
	return range_command(fd, BLKZEROOUT, FALLOC_FL_ZERO_RANGE, start, len);
}

int devfd_discard(int fd, u64 start, u64 len)
{
	return range_command(fd, BLKDISCARD, FALLOC_FL_PUNCH_HOLE, start, len);
}
