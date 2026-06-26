/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* canonicalize_file_name() */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/crc64.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"

#include "shared/devfd.h"
#include "shared/format-block.h"
#include "shared/format-dev-log.h"
#include "shared/log.h"

#include "cli/cli.h"

static double units_value(double x)
{
	while (x >= 1024)
		x /= 1024;
	return x;
}

static char *units_str(double x)
{
	static char *unit_strings[] = {
		"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB", "RiB", "QiB",
		NULL,
	};
	char **u = &unit_strings[0];

	while (x >= 1024 && *u) {
		x/= 1024;
		u++;
	}

	return *u ?: "¯\\_(ツ)_/¯";
}

#define SZ_F		"%6.2f %s"
#define SZ_A(sz)	units_value(sz), units_str(sz)

#define BS_F		"%12zu %3.0f%s blocks, %6.2f %s"
#define BS_A(b, bs)	(size_t)(b), units_value(bs), units_str(bs), units_value((b) * (bs)), \
			units_str((b) * (bs))

#define PCT_F		"%6.2f%%"
#define PCT_A(a, b)	((double)(a) * 100 / (b))

static int write_block_and_sync(int fd, void *buf, u64 offset)
{
	struct rpdfs_log_block_header *hdr = buf;
	u64 crc;
	int ret;

	hdr->dev_addr = cpu_to_le64(offset);

	hdr->crc = 0;
	crc = crc64_nvme(0, hdr, RPDFS_BLOCK_SIZE);
	hdr->crc = cpu_to_le64(crc);

	ret = pwrite(fd, hdr, RPDFS_BLOCK_SIZE, offset);
	if (ret != RPDFS_BLOCK_SIZE) {
		if (ret >= 0)
			ret = -EIO;
		else
			ret = -errno;
		printf("write error: "ENOF"\n", ENOA(-ret));
		goto out;
	}

	ret = fdatasync(fd);
	if (ret != 0) {
		ret = -errno;
		printf("fdatasync error: "ENOF"\n", ENOA(-ret));
		goto out;
	}

	ret = 0;
out:
	return ret;
}

static int format_device_func(int argc, char **argv)
{
	struct rpdfs_log_commit_block *cmt = NULL;
	char uuid_str[UUID_STR_LEN];
	u64 size;
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

	cmt = calloc(1, RPDFS_BLOCK_SIZE);
	if (!cmt) {
		ret = -errno;
		printf("error allocating block buffer: "ENOF"\n", ENOA(-ret));
		goto out;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		ret = -errno;
		printf("error opening '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	ret = devfd_get_size(fd, &size);
	if (ret < 0) {
		printf("getting size: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	/* attempt to discard, fine if not supported */
	devfd_discard(fd, 0, size);

	uuid_generate(cmt->hdr.uuid);
	cmt->hdr.type = RPDFS_LOG_BLOCK_TYPE_COMMIT;
	cmt->commit_seq = cpu_to_le64(1);

	ret = write_block_and_sync(fd, cmt, 0);
	if (ret < 0) {
		printf("error writing root block '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	uuid_unparse(cmt->hdr.uuid, uuid_str);

	printf("Formatted rpdfs device:\n"
	       "  uuid:                 %s\n"
	       "  device size:          "SZ_F"\n",
	       uuid_str, SZ_A(size));

	ret = 0;
out:
	free(cmt);
	if (fd >= 0)
		close(fd);

	return ret;
}

static struct cli_command format_device_cmd = {
	.func = format_device_func,
	.name = "format-device",
	.desc = "Write to device for identification by rpdfs, (destructive!)"
};

CLI_REGISTER(format_device_cmd);
