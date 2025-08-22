/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_MSG_H
#define RPDFS_SHARED_MSG_H

#include <netinet/in.h>

#include "shared/format-msg.h"

u8 rpdfs_msg_err(int eno);
int rpdfs_msg_errno(u8 err);
int rpdfs_msg_verify_header(struct rpdfs_msg_header *hdr);

#endif
