/* SPDX-License-Identifier: GPL-2.0 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <printf.h>
#include <time.h>
#include <sys/mman.h>

#include "shared/dtracef.h"
#include "shared/string_wrappers.h"

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/kernel.h"
#include "shared/lk/list.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"
#include "shared/lk/stringify.h"
#include "shared/lk/timekeeping.h"

#include "utask/utask.h"

/*
 * A simple single threaded tracing facility.
 *
 * Tracing calls are formatted like printf so that we don't have to keep
 * tracing call sites in sync with an external definition of arguments
 * and formats.
 *
 * Each trace call aims to be inexpensive.  With aggressive macros we
 * boil a trace down to a call that returns a pointer and inline stores
 * of the arguments into that pointer.  This avoids producing and
 * consuming copies of the arguments as varargs.
 */

/* XXX should be configurable */
#define TRACE_BUF_SIZE (16 * 1024 * 1024)

static struct dtracef_instance {
	void *base;
	size_t pos;
	size_t size;

	struct list_head site_list;
	unsigned long next_nr;
	void *map_addr;
	size_t map_size;
	int fd;

} global_dtracef_inst = {
	/* site_list and next_nr are used by registration ctors before _init */
	.site_list = LIST_HEAD_INIT(global_dtracef_inst.site_list),
	.next_nr = 1,
};

bool dtracef_enabled = false;

/*
 * The userspace clock_gettime() call hiding behind the kernel ktime_
 * compat is cheap on platforms with VDSO.  It's just a bit more
 * expensive than a raw rdtsc, which seems worth it to get portable
 * realtime timestamps.
 */
struct ngnfs_dtracef_event *dtracef_get_event(struct dtracef_site *site)
{
	struct dtracef_instance *inst = &global_dtracef_inst;
	size_t size = offsetof(struct ngnfs_dtracef_event, args[site->nr_args]);
	struct ngnfs_dtracef_event *ev;
	size_t remaining;

	remaining = NGNFS_DTRACEF_SEGMENT - (inst->pos % NGNFS_DTRACEF_SEGMENT);
	if (size + sizeof(ev->args[0]) > remaining)
		inst->pos += remaining;

	if (inst->pos == inst->size)
		inst->pos = 0;

	ev = inst->base + inst->pos;
	inst->pos += size;

	ev->nr = cpu_to_le64(site->nr);
	ev->utask_id = cpu_to_le64(utask_current_id());
	ev->realtime_ns = cpu_to_le64(ktime_get_real());
	ev->args[site->nr_args] = 0;

	return ev;
}

static size_t source_size(struct ngnfs_dtracef_source *source)
{
	return roundup(offsetof(struct ngnfs_dtracef_source, strings) +
		       le16_to_cpu(source->name_size) + le16_to_cpu(source->fmt_size),
		       NGNFS_DTRACE_SOURCE_ALIGN);
}

static size_t max_source_size(void)
{
	struct ngnfs_dtracef_source source = {
		.name_size = cpu_to_le16(U16_MAX),
		.fmt_size = cpu_to_le16(U16_MAX),
	};

	return source_size(&source);
}

static char *source_name(struct ngnfs_dtracef_source *source)
{
	return (char *)source->strings;
}

static char *source_fmt(struct ngnfs_dtracef_source *source)
{
	return source_name(source) + le16_to_cpu(source->name_size);
}

/*
 * Return a pointer to the next '%' in the string that isn't immediately
 * followed by another '%'.
 */
static void *next_spec(char *str)
{
	while ((str = index(str, '%')) && *(str + 1) == '%')
		str += 2;

	return str;
}

/*
 * Return the number of format specifiers in the string.
 */
static size_t count_specs(char *str)
{
	size_t count = 0;

	while ((str = next_spec(str))) {
		count++;
		str++;
	}

	return count;
}

/*
 * This is the printing converse of the __store_args storage of each
 * argument.  We use the result of glibc's printf format parsing to
 * build a vararg argument that'll satisfy the format spec.
 */
static int print_one(char *fmt, int argtype, u64 arg)
{
	switch(argtype) {
	case PA_INT:				printf(fmt, (unsigned int)arg); break;
	case PA_INT | PA_FLAG_LONG:		printf(fmt, (unsigned long)arg); break;
	case PA_INT | PA_FLAG_LONG_LONG:	printf(fmt, (unsigned long long)arg); break;
	case PA_INT | PA_FLAG_SHORT:		printf(fmt, (unsigned short)arg); break;
	case PA_CHAR:				printf(fmt, (unsigned char)arg); break;
	case PA_POINTER:			printf(fmt, (void *)(intptr_t)arg); break;
	case PA_FLOAT:				printf(fmt, (float)arg); break;
	case PA_DOUBLE:				printf(fmt, (double)arg); break;
	case PA_DOUBLE | PA_FLAG_LONG:		printf(fmt, (long double)arg); break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * Print a trace event.  We separate the string into sections that each
 * have one format and one argument so that we can feasibly reconstruct
 * the vararg argument for each format.
 */
static int print_event(struct ngnfs_dtracef_source *source, struct ngnfs_dtracef_event *ev)
{
	struct timespec ts;
	char *str = NULL;
	int argtype;
	char *sep;
	char *pr;
	int ret;
	int a;

	str = strdup(source_fmt(source));
	if (!str) {
		ret = -ENOMEM;
		goto out;
	}

	/* setup first iteration to print from start to second sep */
	sep = next_spec(str);
	if (!sep) {
		ret = -EINVAL;
		goto out;
	}

	ts.tv_sec = le64_to_cpu(ev->realtime_ns) / NSEC_PER_SEC;
	ts.tv_nsec = le64_to_cpu(ev->realtime_ns) % NSEC_PER_SEC;

	printf("[%llu.%09lu] %llu %s: ",
		(unsigned long long)ts.tv_sec, (unsigned long)ts.tv_nsec,
		ev->utask_id, source_name(source));

	pr = str;
	a = 0;

	/* each loop prints from the previous sep to the next */
	while (pr) {
		sep = next_spec(sep + 1);
		if (sep)
			*sep = '\0';

		ret = parse_printf_format(pr, 1, &argtype);
		if (ret != 1) {
			ret = -EINVAL;
			goto out;
		}

		ret = print_one(pr, argtype, le64_to_cpu(ev->args[a]));
		if (ret < 0)
			goto out;
		a++;

		if (sep)
			*sep = '%';
		pr = sep;
	}

	putchar('\n');
	ret = 0;
out:
	free(str);
	return ret;
}

/*
 * Find the oldest segment in the trace.  Timestamps increase as we
 * advance around the ring.  Once we find the event that decreases we've
 * found the oldest.
 */
static loff_t find_oldest(void *addr, size_t size)
{
	struct ngnfs_dtracef_event *ev;
	struct ngnfs_dtracef_event *end;
	u64 oldest;
	u64 off;

	ev = addr;
	end = addr + rounddown(size, sizeof(struct ngnfs_dtracef_event));
	oldest = le64_to_cpu(ev->realtime_ns);
	off = 0;

	while (ev <= end) {
		if (ev->nr == 0)
			break;

		if (le64_to_cpu(ev->realtime_ns) < oldest) {
			oldest = le64_to_cpu(ev->realtime_ns);
			off = (void *)ev - addr;
			break;
		}

		ev = (void *)ev + NGNFS_DTRACEF_SEGMENT;
	}

	return off;
}

int dtracef_print_trace_mem(void *addr, size_t size)
{
	struct ngnfs_dtracef_source **sources = NULL;
	struct ngnfs_dtracef_source *source;
	struct ngnfs_dtracef_file *fil;
	struct ngnfs_dtracef_event *ev;
	void *ring_addr;
	size_t ring_size;
	size_t start;
	size_t pos;
	u16 nr_sources;
	u16 nr;
	int ret;
	int i;

	if (size < sizeof(struct ngnfs_dtracef_file)) {
		ret = -EINVAL;
		goto out;
	}

	fil = addr;
	if (le64_to_cpu(fil->magic) != NGNFS_DTRACEF_FILE_MAGIC) {
		ret = -EINVAL;
		goto out;
	}

	nr_sources = le16_to_cpu(fil->nr_sources);
	sources = calloc(nr_sources, sizeof(sources[0]));
	if (!sources) {
		ret = -errno;
		goto out;
	}

	/* Verify source headers, saving pointers to each, 0 source is unused */
	source = (void *)(fil + 1);
	for (i = 1; i < nr_sources; i++) {

		if (((void *)source + sizeof(struct ngnfs_dtracef_source)) > (addr + size) ||
		    ((void *)source + source_size(source)) > (addr + size)) {
			ret = -EINVAL;
			goto out;
		}

		/* basic source sanity, no dupe nrs, XXX should verify more */
		nr = le16_to_cpu(source->nr);
		if (le16_to_cpu(source->name_size) < 2 ||
		    le16_to_cpu(source->fmt_size) < 2 ||
		    le16_to_cpu(source->nr_args) > NGNFS_DTRACEF_MAX_ARGS ||
		    (nr >= nr_sources || sources[nr])) {
			ret = -EINVAL;
			goto out;
		}

		/* strings are null terminated */
		if ((source_name(source)[le16_to_cpu(source->name_size) - 1] != '\0') ||
		    (source_fmt(source)[le16_to_cpu(source->fmt_size) - 1] != '\0')) {
			ret = -EINVAL;
			goto out;
		}

		sources[nr] = source;
		source = (void *)source + source_size(source);
	}

	ring_addr = source;
	ring_size = size - (ring_addr - addr);

	start = find_oldest(ring_addr, ring_size);
	pos = start;
	do {
		if (pos >= ring_size) {
			pos = 0;
			continue;
		}

		ev = ring_addr + pos;
		if (ev->nr == 0) {
			pos = roundup(pos + 1, NGNFS_DTRACEF_SEGMENT);
			continue;
		}

		if (le64_to_cpu(ev->nr) >= nr_sources) {
			ret = -EINVAL;
			goto out;
		}

		source = sources[le64_to_cpu(ev->nr)];
		ret = print_event(source, ev);

		pos += offsetof(struct ngnfs_dtracef_event, args[le16_to_cpu(source->nr_args)]);

	} while (pos != start);

	ret = 0;
out:
	free(sources);

	return ret;
}

static const char *check_format(char *fmt)
{
	static int argtypes[NGNFS_DTRACEF_MAX_ARGS];
	size_t len;
	size_t nr;
	size_t n;
	int type;
	int flags __unused;
	int i;

	len = strlen(fmt);
	if (len == 0)
		return "is zero length";

	if (fmt[len - 1] == '\n' || fmt[len - 1 ] == '\r')
		return "ends in newline or carriage return";

	nr = parse_printf_format(fmt, ARRAY_SIZE(argtypes), argtypes);
	if (nr == 0)
		return "contains no format specification";
	if (nr > NGNFS_DTRACEF_MAX_ARGS)
		return "contains more than " __stringify(NGNFS_DTRACEF_MAX_ARGS) " format specifications";

	/* can't have formats with multiple args, like '%*' */
	n = count_specs(fmt);
	if (n != nr)
		return "expects more arguments than format specifiers ('%*'?)";

	/* not supporting wide chars or strings */
	for (i = 0; i < nr; i++) {
		/* look for PA_ at the end of <printf.h> */
		type = argtypes[i] & ~PA_FLAG_MASK;
		flags = argtypes[i] & PA_FLAG_MASK;

		if (type == PA_WCHAR || type == PA_STRING || type == PA_WSTRING)
			return "contains unsupported argument type (wchar, string, or wstring)";
	}

	return NULL;
}

/*
 * We mmap the trace buffer in an allocated file so that if we crash the
 * traces are left behind in the file.  It's an expedient convenience,
 * but this can lead to stores to the trace buffer blocking on
 * writeback, so this isn't a great long term solution.
 */
int dtracef_init(char *path)
{
	struct dtracef_instance *inst = &global_dtracef_inst;
	struct ngnfs_dtracef_source *source;
	struct ngnfs_dtracef_file *fil;
	struct dtracef_site *site;
	const char *msg;
	void *addr = NULL;
	void *buf = NULL;
	size_t file_size;
	size_t sz;
	u16 name_size;
	u16 fmt_size;
	int err;
	int ret;
	int fd = -1;

	err = 0;
	list_for_each_entry(site, &inst->site_list, head) {
		msg = check_format(site->fmt);
		if (msg) {
			printf("invalid format string %s, at %s:%u: '%s'\n",
			       msg, site->file, site->line, site->fmt);
			err = -EINVAL;
		}
	}
	if (err < 0) {
		ret = err;
		goto out;
	}

	buf = malloc(max_source_size());
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	fil = buf;
	fil->magic = cpu_to_le64(NGNFS_DTRACEF_FILE_MAGIC);
	fil->size = 0;
	memset_zero_sizeof(fil->pad_);
	fil->nr_sources = cpu_to_le16(inst->next_nr);

	file_size = sizeof(struct ngnfs_dtracef_file);
	ret = write(fd, fil, file_size);
	if (ret != file_size) {
		if (ret >= 0)
			ret = -EIO;
		else
			ret = -errno;
		goto out;
	}

	source = buf;

	list_for_each_entry(site, &inst->site_list, head) {
		name_size = strlen(site->name) + 1;
		fmt_size = strlen(site->fmt) + 1;

		source->name_size = cpu_to_le16(name_size);
		source->fmt_size = cpu_to_le16(fmt_size);
		source->nr = cpu_to_le16(site->nr);
		source->nr_args = cpu_to_le16(site->nr_args);

		/* cheeky zeroing of potential final padding before overwriting with strings */
		sz = min(NGNFS_DTRACE_SOURCE_ALIGN - 1, name_size + fmt_size);
		memset((void *)source + source_size(source) - sz, 0, sz);

		memcpy(source->strings, site->name, name_size);
		memcpy(source->strings + name_size, site->fmt, fmt_size);

		sz = source_size(source);
		ret = write(fd, source, sz);
		if (ret != sz) {
			if (ret >= 0)
				ret = -EIO;
			else
				ret = -errno;
			goto out;
		}

		file_size += sz;
	}

	file_size += TRACE_BUF_SIZE;

	ret = posix_fallocate(fd, 0, file_size);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	addr = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
	if (addr == MAP_FAILED) {
		ret = -errno;
		goto out;
	}

	fil = addr;
	fil->size = cpu_to_le64(file_size);

	inst->size = TRACE_BUF_SIZE;
	inst->base = addr + file_size - inst->size;
	inst->pos = 0;

	inst->map_addr = addr;
	inst->map_size = file_size;
	inst->fd = fd;

	dtracef_enabled = true;
	ret = 0;
out:
	free(buf);

	if (ret < 0) {
		if (fd >= 0)
			close(fd);
		if (addr && addr != MAP_FAILED)
			munmap(addr, file_size);
	}
	return ret;
}

void dtracef_exit(void)
{
	struct dtracef_instance *inst = &global_dtracef_inst;

	if (inst->map_addr) {
		munmap(inst->map_addr, inst->map_size);
		close(inst->fd);

		inst->next_nr = 1;
		inst->map_addr = NULL;
		inst->fd = -1;

		dtracef_enabled = false;
	}
}

void dtracef_register_site(struct dtracef_site *site)
{
	struct dtracef_instance *inst = &global_dtracef_inst;

	site->nr = inst->next_nr++;
	list_add_tail(&site->head, &inst->site_list);
}
