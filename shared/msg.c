/* SPDX-License-Identifier: GPL-2.0 */

#include <stdlib.h>
#include <errno.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/limits.h"

#include "shared/msg.h"

/*
 * This, perhaps too generously, accepts both positive and negative
 * errno.
 */
u8 rpdfs_msg_err(int eno)
{
#define err_case(e) \
	case e: return RPDFS_MSG_ERR_##e;

	switch (abs(eno)) {
		case 0: return RPDFS_MSG_ERR_OK;
		err_case(ENOMEM)
		default: err_case(EIO)
	}
}

/* return -ve errno from our over-the-wire err */
int rpdfs_msg_errno(u8 err)
{
#define eno_case(e) \
	[RPDFS_MSG_ERR_##e] = e,

	static int eno[] = {
		eno_case(EIO)
		eno_case(ENOMEM)
	};

	switch (err) {
		case RPDFS_MSG_ERR_OK:			return 0;
		case RPDFS_MSG_ERR__INVALID ... U8_MAX:	return -EPROTO;
		default:				return -eno[err];
	}
}

int rpdfs_msg_verify_header(struct rpdfs_msg_header *hdr)
{
	if (hdr->ctl_size > RPDFS_MSG_MAX_CTL_SIZE ||
	    le16_to_cpu(hdr->data_size) > RPDFS_MSG_MAX_DATA_SIZE ||
	    hdr->type >= RPDFS_MSG__NR)
		return -EINVAL;

	return 0;
}
