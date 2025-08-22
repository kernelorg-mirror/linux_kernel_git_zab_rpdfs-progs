/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_MSG_H
#define RPDFS_SHARED_MSG_H

#include <netinet/in.h>

#include "shared/format-msg.h"
#include "shared/fs_info.h"
#include "shared/lk/atomic.h"
#include "shared/lk/gfp.h"
#include "shared/lk/types.h"

/*
 * These message descriptors are only valid for the life of a call, any
 * reference to this data by the callee after returning must be copied
 * out.  These only exist to avoid having a billion argument copies in
 * each frame up and down the call stack.
 */
struct rpdfs_msg_desc {
	struct sockaddr_in *addr;
	void *ctl_buf;
	struct page *data_page;
	u16 data_size;
	u8 ctl_size;
	u8 type;
};

struct rpdfs_msg_transport_ops {
	void *(*setup)(struct rpdfs_fs_info *nfi, void *arg);
	void (*shutdown)(struct rpdfs_fs_info *nfi, void *mtr_info);
	void (*destroy)(struct rpdfs_fs_info *nfi, void *mtr_info);
	void *(*start_listen)(struct rpdfs_fs_info *nfi, struct sockaddr_in *addr);
	void (*stop_listen)(struct rpdfs_fs_info *nfi, void *info);

	size_t peer_info_size;
	void (*init_peer)(void *info, struct rpdfs_fs_info *nfi);
	void (*destroy_peer)(void *info);
	int (*start)(void *info, struct sockaddr_in *addr, void *accepted);
	int (*send)(void *info, struct rpdfs_msg_desc *mdesc);
};

u8 rpdfs_msg_err(int eno);
int rpdfs_msg_errno(u8 err);

int rpdfs_msg_verify_header(struct rpdfs_msg_header *hdr);

int rpdfs_msg_send(struct rpdfs_fs_info *nfi, struct rpdfs_msg_desc *mdesc);
int rpdfs_msg_recv(struct rpdfs_fs_info *nfi, struct rpdfs_msg_desc *mdesc);
int rpdfs_msg_accept(struct rpdfs_fs_info *nfi, struct sockaddr_in *addr, void *arg);

/*
 * The receive path does basic checks of the incoming receive packet.
 * The ctl and data sizes will match the size of the buffers in the
 * desc.  The type will be valid in that it determines which recv method
 * to call.  The recv method is responsible for all other checks.
 */
typedef int (rpdfs_msg_recv_fn_t)(struct rpdfs_fs_info *nfi, struct rpdfs_msg_desc *mdesc);

int rpdfs_msg_register_recv(struct rpdfs_fs_info *nfi, u8 type, rpdfs_msg_recv_fn_t fn);
void rpdfs_msg_unregister_recv(struct rpdfs_fs_info *nfi, u8 type, rpdfs_msg_recv_fn_t fn);

int rpdfs_msg_setup(struct rpdfs_fs_info *nfi, struct rpdfs_msg_transport_ops *mtr_ops,
		    void *setup_arg, struct sockaddr_in *listen_addr);
void rpdfs_msg_destroy(struct rpdfs_fs_info *nfi);

#endif
