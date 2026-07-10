/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_LOG_H
#define RPDFS_SHARED_LOG_H

#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#define ENOF		"%s (errno %d)"
#define ENOA(eno)	strerror(eno), eno

#define IPV4F		"%u.%u.%u.%u:%u"
#define IPV4A(addr)					\
	ntohl((addr)->sin_addr.s_addr) >> 24,		\
	(ntohl((addr)->sin_addr.s_addr) >> 16) & 0xff,	\
	(ntohl((addr)->sin_addr.s_addr) >> 8) & 0xff,	\
	ntohl((addr)->sin_addr.s_addr) & 0xff,		\
	ntohs((addr)->sin_port)

#define log(fmt, args...) \
	dprintf(STDOUT_FILENO, fmt"\n", ##args)

extern bool log_enabled;

#define log_err(fmt, args...) \
do { \
	if (log_enabled) \
		log_line("error: "fmt"\n", ##args); \
} while (0)

void log_line(char *fmt, ...);

int log_init(char *path);
void log_exit(void);

#endif
