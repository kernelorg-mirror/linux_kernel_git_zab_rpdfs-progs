/* SPDX-License-Identifier: GPL-2.0 */

#include <sys/random.h>

#include "shared/lk/bug.h"

#include "shared/get_random.h"

void get_random(void *buf, size_t buflen)
{
	ssize_t ret;

	while (buflen > 0) {
		ret = getrandom(buf, buflen, 0);
		BUG_ON(ret <= 0);

		buf += ret;
		buflen -= ret;
	} while (buflen > 0);
}
