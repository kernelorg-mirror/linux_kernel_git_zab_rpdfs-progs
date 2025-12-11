/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/minmax.h"

#include "shared/compare.h"
#include "shared/format-block.h"
#include "shared/format-dev.h"
#include "shared/format-msg.h"
#include "shared/hash_table.h"
#include "shared/msg.h"
#include "shared/string_wrappers.h"

#include "utask/net.h"
#include "utask/utask.h"

#include "devd/bstore.h"
#include "devd/cache-mode.h"

/*
 * This governs the cached block modes that clients are allowed in their
 * caches across the network.
 *
 * The protocol and this implementation's state machine are built around
 * relatively simplified functionality.  Each block's state is fully
 * independent of others.  The client can only request increased cache
 * mode levels (none->read->write).  The server grants those requests
 * and can also send unsolicited strictly decreasing revokes of
 * previously granted modes.  The client won't request a lesser mode,
 * and the server won't increase the mode without a request.  This
 * simplifies the state transitions that the client and server have to
 * implement.
 *
 * We then reduce round trips by combining the request for a cache mode
 * with a read of the block contents.  The client only sends a read
 * message when it needs the block contents, and it can further include
 * in that message that it is requesting a higher mode.  The server
 * always responds to read with block data, perhaps only metadata, and
 * can optionally be granting a mode.
 *
 * Write commands have a similar bundling, but there the write can
 * optionally also be acknowledging a message from the server to revoke
 * its write mode.  The server processes the acked mode only if the
 * write succeeds.
 *
 * Through all this, we're requiring in-order messaging to ensure that
 * back-to-back sends don't end up processed out of order on the
 * receiver.  This avoids round-trips to serialize the two messages or
 * managing windows of out-of-order received messages to be processed
 * once their sequence is resolved.
 *
 * A core challenge here is that the amount of memory we're willing to
 * spend tracking the mode of cached blocks limits the cache size of the
 * clients.  We want our tracking to be as dense as possible to make the
 * most of our memory resource and allow the largest caches in clients.
 */

static struct cache_mode_instance {
	struct hash_table *ht;
} global_cache_mode_inst = {
};

/*
 * This is built to conserve space by having a small dense client id.
 * Today the net layer uses full addresses so the client_state struct is
 * a lot bigger than it would otherwise be.  This can be updated as we
 * add maps and the infrastructure for changing ids over time.
 */
struct block_state {
	u64 bnr;
	u16 size;
	u16 request_index;
	u16 reading:1;
	struct client_state {
		struct sockaddr_in addr;
		u8 request:RPDFS_CACHE_MODE__BITS,
		    grant:RPDFS_CACHE_MODE__BITS,
		    revoke:RPDFS_CACHE_MODE__BITS,
		    is_read:1,
		    read_data:1;
	} clients[0];
};

/*
 * Iterate over all the present clients in the client array.
 */
#define for_each_client(cli_, bst_) \
	for(cli_ = &(bst_)->clients[0]; cli_ < &(bst_)->clients[(bst_)->size]; cli_++)

/* this is clamped to the array size so iterators can wrap with (cli + 1) */
static inline unsigned long cli_index(struct block_state *bst, struct client_state *cli)
{
	return (cli - bst->clients) % bst->size;
}

/*
 * Iterate over all the clients, starting at a given (wrapped) index,
 * visiting each once.
 */
#define for_each_client_from(cli_, bst_, ind_) \
	for(cli_ = &(bst_)->clients[((ind_) % bst->size)]; \
	    cli_ != NULL; \
	    cli_ = (cli_index((bst_), cli_ + 1) == ((ind_) % bst->size)) ? NULL : \
			&(bst_)->clients[cli_index((bst_), cli_ + 1)])

/*
 * Returns true if the two modes can both be granted for the same block
 * to different clients.  Only writes are incompatible with other reads
 * or writes.  All other combinations, including writes with null or
 * none, are compatible.
 */
static unsigned int compatible_modes(u8 a, u8 b)
{
	if (a > b)
		swap(a, b);

	return b < RPDFS_CACHE_MODE_WRITE || a <= RPDFS_CACHE_MODE_NONE;
}

/*
 * Returns the least restrictive mode that's compatible with the
 * requested mode.  Existing writes conflicting with requested reads are
 * allowed to keep their read cache, but anyone conflicting with
 * requested writes needs to drop their cache so they can later read the
 * result of the write.
 */
static inline u8 most_compatible(u8 mode)
{
	return mode == RPDFS_CACHE_MODE_WRITE ? RPDFS_CACHE_MODE_NONE : RPDFS_CACHE_MODE_READ;
}

static bool cli_is_none(struct client_state *cli)
{
	return cli->request <= RPDFS_CACHE_MODE_NONE &&
	       cli->grant <= RPDFS_CACHE_MODE_NONE &&
	       cli->revoke <= RPDFS_CACHE_MODE_NONE;
}

/*
 * As we put the block state we remove any clients that no longer track
 * information and realloc the array to reclaim memory.
 */ 
static void put_block_state(struct cache_mode_instance *inst, struct block_state *bst)
{
	struct client_state *last;
	struct client_state *cli;
	bool shrank = false;
	void *re;

	if (!bst || bst->reading)
		return;

	for_each_client(cli, bst) {
		if (cli_is_none(cli)) {
			last = &bst->clients[bst->size - 1];
			if (cli != last) {
				*cli = *last;
				if (bst->request_index == cli_index(bst, last))
					bst->request_index = cli_index(bst, cli);
			}
			bst->size--;
			cli--;
			shrank = true;
		}
	}

	if (bst->size == 0) {
		htable_delete(inst->ht, bst->bnr);
		free(bst);

	} else if (shrank) {
		re = realloc(bst, offsetof(struct block_state, clients[bst->size]));
		if (re) {
			bst = re;
			htable_insert(inst->ht, bst->bnr, (u64)bst);
		}
	}
}

static bool addrs_equal(struct sockaddr_in *a, struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
	       a->sin_port == b->sin_port &&
	       a->sin_family == b->sin_family;
}

/*
 * We're relying on the allocator to manage fragmentation of precise
 * allocation.  This lets us put off maintaining our own utilization
 * inside a fixed size pool, which we will probably have to do
 * eventually.  For now burn cpu to realloc the block state with its
 * array each time its size changes.  This is the growth side, put_
 * shrinks.
 */
static int get_block_client(struct cache_mode_instance *inst, u64 bnr, struct sockaddr_in *addr,
			    struct block_state **bst_ret, struct client_state **cli_ret)
{
	struct block_state *bst = NULL;
	struct client_state *cli = NULL;
	void *re;
	int ret;

	bst = (struct block_state *)htable_lookup(inst->ht, bnr);
	if (!bst) {
		bst = malloc(offsetof(struct block_state, clients[1]));
		if (!bst) {
			ret = -ENOMEM;
			goto out;
		}

		bst->bnr = bnr;
		bst->size = 1;
		bst->request_index = 0;
		bst->reading = 0;
		htable_insert(inst->ht, bst->bnr, (u64)bst);

		cli = &bst->clients[0];
		goto init_cli;
	}

	for_each_client(cli, bst) {
		if (addrs_equal(&cli->addr, addr)) {
			ret = 0;
			goto out;
		}
	}

	re = realloc(bst, offsetof(struct block_state, clients[bst->size + 1]));
	if (!re) {
		ret = -errno;
		goto out;
	}
	bst = re;

	htable_insert(inst->ht, bst->bnr, (u64)bst);
	cli = &bst->clients[bst->size++];
init_cli:
	*cli = (struct client_state) {
		.addr = *addr,
	};

	ret = 0;
out:
	if (ret < 0) {
		put_block_state(inst, bst);
		bst = NULL;
		cli = NULL;
	}

	*bst_ret = bst;
	*cli_ret = cli;

	return ret;
}

/*
 * We don't spend the memory on strict fifo processing of requests.  We
 * try to process in round-robin order.  We will process a given request
 * until it is resolved before moving on to the next one.
 */
static struct client_state *find_request(struct block_state *bst)
{
	struct client_state *cli = &bst->clients[0];

	for_each_client_from(cli, bst, bst->request_index) {
		if (cli->request >= RPDFS_CACHE_MODE_READ) {
			bst->request_index = cli_index(bst, cli);
			return cli;
		}
	}

	return NULL;
}

static int send_read_result(struct block_state *bst, struct client_state *cli, u8 mode,
			    struct cached_block *cblk, struct rpdfs_block_details *det,
			    int read_ret)
{
	struct rpdfs_msg_block_read_result rr;
	struct rpdfs_msg_header hdr;
	struct page *data_page;

	rr.bnr = cpu_to_le64(bst->bnr);
	rr.wcount = 0;
	rr.grant_mode = mode;
	rr.err = rpdfs_msg_err(read_ret);
	memset_zero_sizeof(rr._pad);

	hdr.ctl_size = sizeof(rr);
	hdr.data_size = 0;
	hdr.type = RPDFS_MSG_BLOCK_READ_RESULT;
	data_page = NULL;

	if (read_ret == 0) {
		rr.wcount = det->write_ctr;
		if (cli->read_data) {
			hdr.data_size = cpu_to_le16(RPDFS_BLOCK_SIZE);
			data_page = block_data_page(cblk);
		}
	}

	return net_send(&cli->addr, &hdr, &rr, data_page);
}

static int send_cache_mode(struct block_state *bst, struct client_state *cli, u8 type, u8 mode)
{
	struct rpdfs_msg_cache_mode cm;
	struct rpdfs_msg_header hdr;

	cm.bnr = cpu_to_le64(bst->bnr);
	cm.mode = mode;
	memset_zero_sizeof(cm._pad);

	hdr.data_size = 0;
	hdr.ctl_size = sizeof(cm);
	hdr.type = type;

	return net_send(&cli->addr, &hdr, &cm, NULL);
}

/*
 * Advance the state machine across all the clients for a given block.
 * The driver of change is incoming requests.  If their requested mode
 * is incompatible with other granted modes then we send revokes.  Once
 * the revokes are confirmed then the request is compatible with other
 * grants and we can send a grant for the request.
 *
 * We take responsibility of putting the bst from the caller.  They
 * can't use their bst once this is called.
 *
 * ->reading acts to pause processing while a utask is reading the
 * block.  While it's reading the block state won't be freed and no
 * further processing will be done.  Once the read is complete it
 * returns to processing and can then send block contents for any read
 * responses.
 */
static void process_requests_and_put(struct cache_mode_instance *inst, struct block_state *bst)
{
	struct cached_block *cblk = NULL;
	struct rpdfs_block_details det;
	struct client_state *cli;
	struct client_state *req;
	bool saw_incompat;
	int read_ret = 0;
	u8 compat;
	int ret;

	if (!bst || bst->reading)
		return;

restart:
	while ((req = find_request(bst))) {

		/* don't process next request until revokes have been confirmed */
		if (req->revoke)
			break;

		compat = most_compatible(req->request);
		saw_incompat = false;

		for_each_client(cli, bst) {
			if (cli == req || compatible_modes(cli->grant, req->request))
				continue;

			saw_incompat = true;

			if (!cli->revoke || compat < cli->revoke) {
				ret = send_cache_mode(bst, cli, RPDFS_MSG_BLOCK_REVOKE_MODE,
						      compat);
				BUG_ON(ret < 0); /* evict?  shutdown? */

				cli->revoke = compat;
			}
		}

		if (saw_incompat)
			break;

		/* logical bst is pinned, but it can be realloced if requests arrive */
		if (req->is_read && cblk == NULL && read_ret == 0) {
			u64 bnr = bst->bnr;

			bst->reading = 1;
			read_ret = bstore_read(bnr, &cblk, &det);
			bst = (struct block_state *)htable_lookup(inst->ht, bnr);
			BUG_ON(bst == NULL); /* should have been pinned by ->reading */
			bst->reading = 0;

			goto restart;
		}

		if (req->is_read)
			ret = send_read_result(bst, req, req->request, cblk, &det, read_ret);
		else
			ret = send_cache_mode(bst, req, RPDFS_MSG_BLOCK_GRANT_MODE, req->request);
		BUG_ON(ret < 0); /* evict?  shutdown? */

		req->grant = req->request;
		req->request = RPDFS_CACHE_MODE_NULL;
		req->is_read = 0;
		req->read_data = 0;
	}

	block_put(cblk);
	put_block_state(inst, bst);
}

/*
 * Record a request from the client for a mode for its block.  We'll
 * block until the mode is available for and the caller can send the
 * response message (probably including a read of the block contents.)
 *
 * The client only sends requests for higher modes.  The only time we'll
 * have two requests in flight is if a read was pending and a write
 * arrived.
 */
int cache_mode_request(struct sockaddr_in *addr, u64 bnr, u8 mode, bool is_read, bool with_data)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct block_state *bst = NULL;
	struct client_state *cli;
	int ret;

	/* only support requesting elevated modes for now */
	if (mode < RPDFS_CACHE_MODE_READ) {
		ret = -EINVAL;
		goto out;
	}

	ret = get_block_client(inst, bnr, addr, &bst, &cli);
	if (ret < 0)
		goto out;

	/* shouldn't request dupes, or read while write in flight */
	if (mode <= cli->request) {
		ret = -EINVAL;
		goto out;
	}

	cli->request = mode;
	if (is_read && !cli->is_read) {
		cli->is_read = 1;
		cli->read_data = !!with_data;
	}

	ret = 0;
out:
	process_requests_and_put(inst, bst);
	return ret;
}

/*
 * We can only receive confirms in response to having sent revokes lower
 * the client's granted mode while processing an incompatible request.
 *
 * We can send back-to-back revokes for decreasing modes.  We can send
 * READ, then NONE.  We can receive a corresponding stream of confirms
 * response.
 *
 * We can receive a confirm for an even lesser mode than what we revoked
 * in the case that the client shrank and removed their cached block
 * before they received the revoke.
 */
int cache_mode_confirm(struct sockaddr_in *addr, u64 bnr, u8 mode)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct block_state *bst = NULL;
	struct client_state *cli;
	int ret;

	if (mode < RPDFS_CACHE_MODE_NONE || mode > RPDFS_CACHE_MODE_READ) {
		ret = -EPROTO;
		goto out;
	}

	ret = get_block_client(inst, bnr, addr, &bst, &cli);
	if (ret < 0)
		goto out;

	if (mode > cli->grant || !cli->revoke) {
		ret = -EPROTO;
		goto out;
	}

	cli->grant = mode;
	if (mode <= cli->revoke)
		cli->revoke = RPDFS_CACHE_MODE_NULL;

	ret = 0;

out:
	process_requests_and_put(inst, bst);
	return ret;
}

int cache_mode_init(void)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;

	/* XXX we don't shrink to keep the block state count within this limit */
	inst->ht = htable_alloc(1024 * 1024);
	if (!inst->ht)
		return -ENOMEM;

	return 0;
}

void cache_mode_exit(void)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct block_state *bst;
	unsigned long fe;

	if (inst->ht) {
		htable_foreach_init(inst->ht, &fe);
		while ((bst = (struct block_state *)htable_foreach(inst->ht, &fe)))
			free(bst);
		free(inst->ht);
		inst->ht = NULL;
	}
}
