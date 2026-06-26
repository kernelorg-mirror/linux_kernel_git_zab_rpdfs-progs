/* SPDX-License-Identifier: GPL-2.0 */

#include <string.h>
#include <errno.h>
#include <netinet/in.h>

#include "shared/lk/bitops-le.h"
#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/kernel.h"

#include "shared/format-block.h"
#include "shared/format-msg.h"
#include "shared/msg.h"
#include "shared/string_wrappers.h"

#include "utask/net.h"
#include "utask/utask.h"

#include "devd/rlock.h"
#include "devd/lstore.h"
#include "devd/proc.h"

/*
 * The incoming control buf is allocated just after the proc_request
 * itself.
 */
struct proc_request {
	struct sockaddr_in addr;
	struct rpdfs_msg_header hdr;
	void *ctl_buf;
	struct page *data_page;
};

static void free_proc_request(struct proc_request *preq)
{
	if (preq) {
		if (preq->data_page)
			put_page(preq->data_page);
		free(preq);
	}
}

static int send_hdr(struct sockaddr_in *addr, u8 type, void *ctl_buf, u8 ctl_size,
		    struct page *data_page, u16 data_size)
{
	struct rpdfs_msg_header hdr = {
		.data_size = cpu_to_le16(data_size),
		.ctl_size = ctl_size,
		.type = type,
	};

	return net_send(addr, &hdr, ctl_buf, data_page);
}

static int send_ctl(struct sockaddr_in *addr, u8 type, void *ctl_buf, u8 ctl_size)
{
	return send_hdr(addr, type, ctl_buf, ctl_size, NULL, 0);
}

static void block_read_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_block_read *br = preq->ctl_buf;
	struct rpdfs_msg_block_read_result rr = { .key = br->key, };
	struct blk_handle *hnd;
	int ret;

	hnd = lstore_read(&br->key, &rr.det);
	if (IS_ERR(hnd)) {
		rr.err = rpdfs_msg_err(PTR_ERR(hnd));
		ret = send_ctl(&preq->addr, RPDFS_MSG_BLOCK_READ_RESULT, &rr, sizeof(rr));
	} else {
		ret = send_hdr(&preq->addr, RPDFS_MSG_BLOCK_READ_RESULT, &rr, sizeof(rr),
			       blk_data_page(hnd), RPDFS_BLOCK_SIZE);
	}
	BUG_ON(ret != 0); /* XXX reconnect? timeout? evict? */

	free_proc_request(preq);
}

static void block_write_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_block_write *bw = preq->ctl_buf;
	struct rpdfs_msg_block_write_result wr;
	int ret;

	ret = lstore_write(&bw->key, preq->data_page, RPDFS_BLOCK_SIZE, &bw->det);

	wr.key = bw->key;
	memset_zero_sizeof(wr._pad);
	wr.err = rpdfs_msg_err(ret);

	ret = send_ctl(&preq->addr, RPDFS_MSG_BLOCK_WRITE_RESULT, &wr, sizeof(wr));
	BUG_ON(ret != 0); /* XXX reconnect? timeout? evict? */

	free_proc_request(preq);
}

static void block_counts_request_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_block_counts_result bcr;
	int ret;

	lstore_get_block_counts(&bcr);

	ret = send_hdr(&preq->addr, RPDFS_MSG_BLOCK_COUNTS_RESULT, &bcr, sizeof(bcr), NULL, 0);
	BUG_ON(ret);

	free_proc_request(preq);
 }

static void rlock_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_rlock *acc = preq->ctl_buf;
	int ret;

	switch (preq->hdr.type) {
		case RPDFS_MSG_RLOCK_REQUEST:
			ret = rlock_request(&preq->addr, &acc->key, acc->mode);
			break;
		case RPDFS_MSG_RLOCK_CONFIRM:
			ret = rlock_confirm(&preq->addr, &acc->key, acc->mode);
			break;
		case RPDFS_MSG_RLOCK_RELEASE:
			ret = rlock_release(&preq->addr, &acc->key, acc->mode);
			break;
	}

	BUG_ON(ret);
	free_proc_request(preq);
}

static utask_fn_t proc_utask_fns[] = {
	[RPDFS_MSG_BLOCK_READ] = block_read_utask,
	[RPDFS_MSG_BLOCK_WRITE] = block_write_utask,
	[RPDFS_MSG_BLOCK_COUNTS_REQUEST] = block_counts_request_utask,
	[RPDFS_MSG_RLOCK_REQUEST] = rlock_utask,
	[RPDFS_MSG_RLOCK_CONFIRM] = rlock_utask,
	[RPDFS_MSG_RLOCK_RELEASE] = rlock_utask,
};

/*
 * This is called within a utask.
 */
int proc_recv(struct sockaddr_in *addr, struct rpdfs_msg_header *hdr, void *ctl_buf,
	      struct page *data_page)
{
	struct proc_request *preq;
	struct utask *tsk;
	utask_fn_t fn;
	int ret;

	if (hdr->type >= ARRAY_SIZE(proc_utask_fns) || ((fn = proc_utask_fns[hdr->type]) == NULL)) {
		ret = -EPROTO;
		goto out;
	}

	preq = malloc(sizeof(struct proc_request) + hdr->ctl_size);
	if (!preq) {
		ret = -ENOMEM;
		goto out;
	}

	preq->addr = *addr;
	preq->hdr = *hdr;
	preq->ctl_buf = (preq + 1);
	if (hdr->ctl_size)
		memcpy(preq->ctl_buf, ctl_buf, hdr->ctl_size);
	preq->data_page = data_page;
	if (data_page)
		get_page(data_page);

	ret = utask_create(fn, preq, &tsk);
	if (ret < 0)
		free_proc_request(preq);
	else
		utask_destroy_at_finish(tsk);
out:
	return ret;
}
