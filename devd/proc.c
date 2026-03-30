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

#include "devd/cache-mode.h"
#include "devd/bstore.h"
#include "devd/free-map.h"
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

/*
 * The main block_read handler just hands the request off to the cache
 * mode manager.  It will make sure that other clients have compatible
 * modes before sending the result.  It is responsible for sending the
 * block data along with the result, if necessary.
 */
static void block_read_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_block_read *br = preq->ctl_buf;
	struct rpdfs_msg_block_read_result rr;
	const u64 bnr = le64_to_cpu(br->bnr);
	bool with_data;
	int ret;

	if (br->request_mode < RPDFS_CACHE_MODE_NULL) {
		ret = -EINVAL;
		goto out;
	}

	with_data = (le64_to_cpu(br->flags) & RPDFS_MSG_BLOCK_READ_FLAG_DATA) != 0;

	ret = cache_mode_request(&preq->addr, bnr, br->request_mode, true, with_data);
out:
	if (ret < 0) {
		rr.bnr = cpu_to_le64(bnr);
		rr.grant_mode = RPDFS_CACHE_MODE_NULL;
		rr.err = rpdfs_msg_err(ret);
		ret = send_ctl(&preq->addr, RPDFS_MSG_BLOCK_READ_RESULT, &rr, sizeof(rr));
		/* shutdown on send errors? */
	}

	BUG_ON(ret != 0); /* XXX reconnect? timeout? evict? */

	free_proc_request(preq);
}

/*
 * If the sender is flushing in response to losing their write mode then
 * the write can contain an acked mode.  We only process the ack if the
 * IO succeeds.  The caller will resend writes as long as the block is
 * dirty.  (If they give up and drop the dirty block they can send an
 * ack mode).
 */
static void block_write_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_block_write *bw = preq->ctl_buf;
	struct rpdfs_msg_block_write_result wr;
	struct rpdfs_block_details in_det;
	const u64 bnr = le64_to_cpu(bw->bnr);
	int ret;

	in_det.alloc_ctr = bw->det.alloc_ctr;
	in_det.write_ctr = bw->det.wcount;
	in_det.place_lo = bw->det.place_lo;
	in_det.place_hi = bw->det.place_hi;

	ret = bstore_write(bnr, preq->data_page, &in_det);

	wr.bnr = bw->bnr;
	wr.err = rpdfs_msg_err(ret);
	memset_zero_sizeof(wr._pad);

	ret = send_ctl(&preq->addr, RPDFS_MSG_BLOCK_WRITE_RESULT, &wr, sizeof(wr));
	if (ret == 0 && bw->confirm_mode)
		ret = cache_mode_confirm(&preq->addr, bnr, bw->confirm_mode);

	BUG_ON(ret != 0); /* XXX reconnect? timeout? evict? */

	free_proc_request(preq);
}

static void block_request_mode_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_cache_mode *cm = preq->ctl_buf;
	const u64 bnr = le64_to_cpu(cm->bnr);
	int ret;

	/* XXX verify */

	ret = cache_mode_request(&preq->addr, bnr, cm->mode, false, false);
	BUG_ON(ret);

	free_proc_request(preq);
}

/*
 * Sent by the client once it confirms that its use of the cached block
 * is compatible with its received revoke mode.
 */
static void block_confirm_mode_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_cache_mode *cm = preq->ctl_buf;
	const u64 bnr = le64_to_cpu(cm->bnr);
	int ret;

	/* XXX verify */

	ret = cache_mode_confirm(&preq->addr, bnr, cm->mode);
	BUG_ON(ret);

	free_proc_request(preq);
}

static void free_stripe_request_utask(void *data)
{
	struct proc_request *preq = data;
	struct rpdfs_msg_free_stripe_request *fsr = preq->ctl_buf;
	const size_t stripe_size = RPDFS_MSG_BLOCKS_PER_FREE_STRIPE;
	struct rpdfs_msg_free_stripe_detail *fsd;
	struct rpdfs_msg_free_stripe_grant fsg;
	struct page *data_page = NULL;
	unsigned long *bmap = NULL;
	u16 data_size;
	u64 bnr;
	int b;
	int i;
	int ret;

	memset(&fsg, 0, sizeof(struct rpdfs_msg_free_stripe_grant));

	if (fsr->flags & RPDFS_MSG_FREE_STRIPE_REQUEST_FLAG_SEARCH) {
		fsg.flags = RPDFS_MSG_FREE_STRIPE_GRANT_FLAG_SEARCH;
		ret = free_map_find_first_most(&bnr);
		BUG_ON(ret);
	} else {
		bnr = le64_to_cpu(fsr->bnr);
	}
	fsg.bnr = cpu_to_le64(bnr);

	bmap = calloc(DIV_ROUND_UP(stripe_size, BITS_PER_LONG), sizeof(bmap[0]));
	data_page = alloc_page(GFP_NOFS);
	if (!bmap || !data_page) {
		ret = -ENOMEM;
		goto out;
	}
	fsd = page_address(data_page);

	ret = bstore_get_free_details(bnr, bmap, fsd, stripe_size);
	if (ret > 0)
		ret = cache_mode_grant_bulk_uncached(&preq->addr, RPDFS_CACHE_MODE_WRITE, bnr,
						     bmap, stripe_size);
	if (ret < 0)
		goto out;

	/* translate our native bmap into le and collapse the details */
	for (b = 0, i = 0; (b = find_next_bit(bmap, stripe_size, b)) < stripe_size; b++, i++) {
		set_bit_le(b, fsg.bmap);
		if (i != b)
			fsd[i] = fsd[b];
	}
	data_size = i * sizeof(fsd[0]);

	ret = send_hdr(&preq->addr, RPDFS_MSG_FREE_STRIPE_GRANT, &fsg, sizeof(fsg),
		       data_page, data_size);
	if (ret < 0) {
		cache_mode_undo_bulk_grant(&preq->addr, bnr, bmap, stripe_size);
		goto out;
	}

	ret = 0;
out:
	free(bmap);
	if (data_page)
		put_page(data_page);

	free_proc_request(preq);
}

static utask_fn_t proc_utask_fns[] = {
	[RPDFS_MSG_BLOCK_READ] = block_read_utask,
	[RPDFS_MSG_BLOCK_WRITE] = block_write_utask,
	[RPDFS_MSG_BLOCK_REQUEST_MODE] = block_request_mode_utask,
	[RPDFS_MSG_BLOCK_CONFIRM_MODE] = block_confirm_mode_utask,
	[RPDFS_MSG_FREE_STRIPE_REQUEST] = free_stripe_request_utask,
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
out:
	return ret;
}
