/* SPDX-License-Identifier: GPL-2.0 */

#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "log.h"

static struct log_instance {
	FILE *filp;
} global_log_inst;

bool log_enabled = false;

/*
 * iso8601: readable by humans and computers, explicit timezone, constant width
 *
 * YYYY-MM-DDTHH:MM:SS.sss+ZZ:ZZ
 */
void log_line(char *fmt, ...)
{
	struct log_instance *inst = &global_log_inst;
	struct timeval tv;
	char tfmt[40];
	struct tm tm;
	time_t tt;
	va_list ap;

	gettimeofday(&tv, NULL);
	tt = tv.tv_sec;
	localtime_r(&tt, &tm);

	strftime(tfmt, sizeof(tfmt), "%G-%m-%dT%H:%M:%S.%%03lu%z ", &tm);
	fprintf(inst->filp, tfmt, (long)tv.tv_usec / 1000);

	va_start(ap, fmt);
	vfprintf(inst->filp, fmt, ap);
	va_end(ap);
}

int log_init(char *path)
{
	struct log_instance *inst = &global_log_inst;
	int ret;

	inst->filp = fopen(path, "w");
	if (!inst->filp) {
		fprintf(stderr, "error opening log path '%s': %s\n", path, strerror(errno));
		ret = -errno;
		goto out;
	}

	setlinebuf(inst->filp);

	log_enabled = true;
	ret = 0;
out:
	if (ret < 0)
		log_exit();

	return ret;
}

void log_exit(void)
{
	struct log_instance *inst = &global_log_inst;

	log_enabled = false;

	if (inst->filp)
		fclose(inst->filp);
}
