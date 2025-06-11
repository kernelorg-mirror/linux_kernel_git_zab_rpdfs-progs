/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_DTRACEF_H
#define NGNFS_SHARED_FORMAT_DTRACEF_H

#include "shared/lk/byteorder.h"

/*
 * Our macro shenanigans have a limit of 127 args.
 */
#define NGNFS_DTRACEF_MAX_ARGS	127
/*
 * Events are grouped into segments.  Events never span segments so they can be used
 * for random access.  An event with a nr of 0 in a segment marks the last event
 * stored in a segment.
 */
#define NGNFS_DTRACEF_SEGMENT	8192
/*
 * Each source is padded after its strings to align the next source or first event.
 */
#define NGNFS_DTRACE_SOURCE_ALIGN 8

#define NGNFS_DTRACEF_FILE_MAGIC 0xf0a64c12cf8aed38 /* just random */

/*
 * Traces are collected into files and start with a header.
 */
struct ngnfs_dtracef_file {
	__le64 magic;
	__le64 size;
	__u8 pad_[6];
	__le16 nr_sources;
};

/*
 * After the file header comes eheaders that describe the sources of all
 * the events.  The name and fmt string sizes include their required
 * terminating null.
 */
struct ngnfs_dtracef_source {
	__le16 name_size;
	__le16 fmt_size;
	__le16 nr;
	__le16 nr_args;
	__u8 strings[6]; /* padded to align to 8 bytes */
};

/*
 * Then come the timestamp and arguments for each stored trace event.
 * The nr refers to the source that has the format and number of args
 * for this evente.
 */
struct ngnfs_dtracef_event {
	__le64 nr;
	__le64 utask_id;
	__le64 realtime_ns;
	__le64 args[0];
};

#endif
