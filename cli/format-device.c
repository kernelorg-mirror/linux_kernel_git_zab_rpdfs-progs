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
#include "shared/format-dev.h"
#include "shared/log.h"

#include "cli/cli.h"

static double units_value(double x)
{
	while (x >= 1000)
		x /= 1000;
	return x;
}

static char *units_str(double x)
{
	static char *unit_strings[] = {
		"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB", "RiB", "QiB",
		NULL,
	};
	char **u = &unit_strings[0];

	while (x >= 1000 && *u) {
		x/= 1000;
		u++;
	}

	return *u ?: "¯\\_(ツ)_/¯";
}

#define BS_F		"%12zu %3.0f%s blocks, %6.2f %s"
#define BS_A(b, bs)	(size_t)(b), units_value(bs), units_str(bs), units_value((b) * (bs)), \
			units_str((b) * (bs))

#define PCT_F		"%6.2f%%"
#define PCT_A(a, b)	((double)(a) * 100 / (b))

static void change_flags(int fd, int and, int or)
{
	int fl;

	fl = fcntl(fd, F_GETFL);
	if (fl >= 0)
		fcntl(fd, F_SETFL, (fl & and) | or);
}

/*
 * We saw some nvme devices that didn't implement the discard_zeros
 * quirk and then had tiny limits on write_zeros so the kernel's bldev
 * zeroing (falloc or BLK ioctl) took orders of magnitude longer than
 * just writing zeros :/.
 */
#define ZERO_SIZE (32 * 1024 * 1024)
static int write_zeros(int orig_fd, off_t start, size_t len)
{
	char *buf = NULL;
	bool have_direct;
	int fd = -1;
	size_t part;
	int ret;

	buf = calloc(1, ZERO_SIZE);
	if (!buf) {
		ret = -errno;
		goto out;
	}

	fd = dup(orig_fd);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	change_flags(fd, ~0, O_DIRECT);
	ret = fcntl(fd, F_GETFL);
	have_direct = ret >= 0 && (ret & O_DIRECT);

	while (len > 0) {
		part = min(ZERO_SIZE, len);
		ret = pwrite(fd, buf, part, start);
		if (ret != part) {
			if (have_direct && (errno == EOPNOTSUPP || errno == EINVAL)) {
				change_flags(fd, ~O_DIRECT, 0);
				have_direct = false;
				continue;
			}
			if (ret >= 0)
				ret = -EIO;
			else
				ret = -errno;
			goto out;
		}
		if (!have_direct)
			posix_fadvise(fd, start, part, POSIX_FADV_DONTNEED);
		start += part;
		len -= part;
	}

out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return ret;
}

static int format_device_func(int argc, char **argv)
{
	struct rpdfs_dev_commit_block *cmt = NULL;
	char uuid_str[37];
	uuid_t uuid;
	u64 size;
	u64 total;
	u64 commit;
	u64 journal;
	u64 summary;
	u64 details;
	u64 internal;
	u64 store;
	u64 crc;
	u64 d;
	u64 s;
	int fd = -1;
	int off;
	int ret;

	/*
	 * XXX how are we doing option parsing in commands?
	 */
	if (argc != 2) {
		printf("incorrect argc %d\n", argc);
		ret = -EINVAL;
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

	total = size / RPDFS_BLOCK_SIZE;
	journal = total >> 10;
	commit = journal >> 2;
	journal -= commit;

	if (commit < RPDFS_DEV_MIN_JC_BLOCKS) {
		printf("device with %llu 4KiB blocks results in %llu commit blocks.\n"
		       "%u commit blocks are required, which is met by a device with %llu blocks\n",
		       total, commit, RPDFS_DEV_MIN_JC_BLOCKS,
		       ((u64)RPDFS_DEV_MIN_JC_BLOCKS) << 12);
		ret = -EINVAL;
		goto out;
	}

	summary = 1;
	details = 1;
	store = total;
	do {
		d = details;
		s = summary;
		/*
		 * We clamp the available fs storage blocks to be a
		 * whole multiple of the number of blocks described by
		 * full summary and details blocks.  We don't need users
		 * of the summary and details blocks to test block
		 * bounds or for entries that are recorded as unused.
		 */
		store = rounddown(total - commit - journal - summary - details,
				  RPDFS_DEV_SUMMARIES_PER_BLOCK * RPDFS_DEV_DETAILS_PER_BLOCK);
		details = DIV_ROUND_UP(store, RPDFS_DEV_DETAILS_PER_BLOCK);
		summary = DIV_ROUND_UP(details, RPDFS_DEV_SUMMARIES_PER_BLOCK);
	} while (d != details || s != summary);

	/* attempt to discard, fine if not supported */
	devfd_discard(fd, 0, size);

	/* initialized metadata must be zero, however */
	ret = write_zeros(fd, 0, (total - store) * RPDFS_BLOCK_SIZE);
	if (ret < 0) {
		printf("zeroing device metadata blocks: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	cmt = calloc(1, RPDFS_BLOCK_SIZE);
	cmt->hdr.type = RPDFS_DEV_BLOCK_TYPE_COMMIT;
	cmt->layout.commit_blocks = cpu_to_le64(commit);
	cmt->layout.journal_blocks = cpu_to_le64(journal);
	cmt->layout.summary_blocks = cpu_to_le64(summary);
	cmt->layout.details_blocks = cpu_to_le64(details);
	cmt->layout.storage_blocks = cpu_to_le64(store);

	uuid_generate_random(uuid);
	uuid_unparse(uuid, uuid_str);
	BUILD_BUG_ON(RPDFS_UUID_SIZE != sizeof(uuid_t));
	memcpy(&cmt->hdr.dev_uuid, &uuid, RPDFS_UUID_SIZE);

	off = sizeof_field(struct rpdfs_dev_block_header, crc);
	crc = crc64_nvme(0, (void *)cmt + off, RPDFS_BLOCK_SIZE - off);
	cmt->hdr.crc = cpu_to_le64(crc);

	ret = pwrite(fd, cmt, RPDFS_BLOCK_SIZE, 0);
	if (ret != RPDFS_BLOCK_SIZE) {
		if (ret >= 0)
			ret = -EIO;
		else
			ret = -errno;
		printf("write error: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	ret = fdatasync(fd);
	if (ret != 0) {
		ret = -errno;
		printf("fdatasync error: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	internal = commit + journal + summary + details;
	printf("Formatted rpdfs device:\n"
	       "  uuid:                 %s\n"
	       "  device size:          "BS_F"\n"
	       "  internal metadata:    "BS_F" ("PCT_F")\n"
	       "  usable fs blocks:     "BS_F" ("PCT_F")\n"
	       "  unused end of device: "BS_F" ("PCT_F")\n",
		uuid_str,
		BS_A(total, RPDFS_BLOCK_SIZE),
		BS_A(internal, RPDFS_BLOCK_SIZE), PCT_A(internal, total),
	        BS_A(store, RPDFS_BLOCK_SIZE), PCT_A(store, total),
		BS_A(total - (internal + store), RPDFS_BLOCK_SIZE),
			PCT_A(total - (internal + store), total));

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
