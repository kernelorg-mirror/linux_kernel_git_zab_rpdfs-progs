/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_DEVD_PROC_H
#define NGNFS_DEVD_PROC_H

int proc_recv(struct sockaddr_in *addr, struct ngnfs_msg_header *hdr, void *ctl_buf,
	      struct page *data_page);

#endif
