/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DEVD_PROC_H
#define RPDFS_DEVD_PROC_H

int proc_recv(struct sockaddr_in *addr, struct rpdfs_msg_header *hdr, void *ctl_buf,
	      struct page *data_page);

#endif
