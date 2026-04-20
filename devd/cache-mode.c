/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "shared/lk/bitops-le.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/jhash.h"
#include "shared/lk/minmax.h"
#include "shared/lk/rbtree.h"

#include "shared/compare.h"
#include "shared/dtracef.h"
#include "shared/format-block.h"
#include "shared/format-dev.h"
#include "shared/format-msg.h"
#include "shared/get_random.h"
#include "shared/msg.h"
#include "shared/string_wrappers.h"

#include "utask/net.h"
#include "utask/utask.h"

#include "devd/bstore.h"
#include "devd/cache-mode.h"
#include "devd/free-map.h"

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
 * once their sequence is resolved.  (Sending a grant and then revoke is
 * the troubling case here.)
 */

static struct cache_mode_instance {
	struct rb_root client_blocks_root;
	struct list_head lru_list;
	u64 nr_blocks;
	u64 nr_revoke_none;
	u64 block_limit;

} global_cache_mode_inst = {
	.client_blocks_root = RB_ROOT,
	.lru_list = LIST_HEAD_INIT(global_cache_mode_inst.lru_list),
	.block_limit = 256 * 1024,
};

struct client_block {
	struct rb_node node;
	struct list_head lru_head;
	u64 bnr;
	u32 ipv4_addr;
	u16 ipv4_port;
	u16 request:RPDFS_CACHE_MODE__BITS,
	    grant:RPDFS_CACHE_MODE__BITS,
	    revoke:RPDFS_CACHE_MODE__BITS,
	    is_read:1,
	    processing:1,
	    read_data:1;
};

#define CLB_ADDR_FMT		"%u.%u.%u.%u:%u"
#define CLB_ADDR_ARG(clb)	(clb)->ipv4_addr >> 24, ((clb)->ipv4_addr >> 16) & 7, \
				((clb)->ipv4_addr >> 8) & 7, (clb)->ipv4_addr & 7,  \
				(clb)->ipv4_port

#define CLB_FMT		"addr "CLB_ADDR_FMT" bnr %llu rq %u gr %u rv %u ir %u pr %u rd %u"
#define CLB_ARG(clb)	CLB_ADDR_ARG(clb), (clb)->bnr, (clb)->request, (clb)->grant, \
			(clb)->revoke, (clb)->is_read, (clb)->processing, (clb)->read_data

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

static void clb_addr_from_sin(struct client_block *clb, struct sockaddr_in *addr)
{
	clb->ipv4_addr = ntohl(addr->sin_addr.s_addr);
	clb->ipv4_port = ntohs(addr->sin_port);
}

static void clb_addr_to_sin(struct sockaddr_in *addr, struct client_block *clb)
{
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = htonl(clb->ipv4_addr);
	addr->sin_port = htons(clb->ipv4_port);
}

static int cmp_client_blocks(struct client_block *a, struct client_block *b)
{
	return rpdfs_compare(a->bnr, b->bnr) ?:
	       rpdfs_compare(a->ipv4_addr, b->ipv4_addr) ?:
	       rpdfs_compare(a->ipv4_port, b->ipv4_port);
}

static inline struct client_block *clb_container(struct rb_node *node)
{
	return node ? container_of(node, struct client_block, node) : NULL;
}

static inline struct client_block *next_clb(struct client_block *clb)
{
	return clb ? clb_container(rb_next(&clb->node)) : NULL;
}

static inline struct client_block *prev_clb(struct client_block *clb)
{
	return clb ? clb_container(rb_prev(&clb->node)) : NULL;
}

static bool only_client_with_block(struct client_block *clb)
{
	struct client_block *nei;

	return (!(nei = next_clb(clb)) || nei->bnr != clb->bnr) &&
	       (!(nei = prev_clb(clb)) || nei->bnr != clb->bnr);
}

static struct client_block *alloc_client_block(struct sockaddr_in *addr, u64 bnr)
{
	struct client_block *clb;

	clb = calloc(1, sizeof(struct client_block));
	if (!clb)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&clb->lru_head);
	clb->bnr = bnr;
	clb_addr_from_sin(clb, addr);

	return clb;
}

static void insert_client_block(struct cache_mode_instance *inst, struct client_block *clb,
				struct rb_node *parent, struct rb_node **link)
{
	rb_link_node(&clb->node, parent, link);
	rb_insert_color(&clb->node, &inst->client_blocks_root);
	list_add_tail(&clb->lru_head, &inst->lru_list);
	inst->nr_blocks++;

	dtracef("cache_mode_insert", "clb %p "CLB_FMT, clb, CLB_ARG(clb));

	if (only_client_with_block(clb))
		free_map_add_cached(clb->bnr, 1);
}

/*
 * A node with ins's position can't exist in the tree.  prev and next
 * must be the existing nodes in the tree that immediately precede or
 * follow ins, if they exist.  With that, we'll always have a null link
 * towards ins unless the tree is empty and we insert at the root.
 *
 * (f.e. consider next.  It can have left link, but ins isn't in the
 * tree.  Any left child has to be less than next.  So it must be prev.
 * Then that prev can't have a right link, 'cause the only node greater
 * than it and less than next must be ins.)
 */
static void insert_between(struct cache_mode_instance *inst, struct client_block *prev,
			   struct client_block *ins, struct client_block *next)
{
	if (prev && !prev->node.rb_right)
		insert_client_block(inst, ins, &prev->node, &prev->node.rb_right);
	else if (next && !next->node.rb_left)
		insert_client_block(inst, ins, &next->node, &next->node.rb_left);
	else
		insert_client_block(inst, ins, NULL, &inst->client_blocks_root.rb_node);
}

static struct client_block *search_client_blocks(struct cache_mode_instance *inst,
						 struct sockaddr_in *addr, u64 bnr,
						 bool alloc, struct client_block **prev,
						 struct client_block **next)
{
	struct rb_node **link = &inst->client_blocks_root.rb_node;
	struct client_block *clb = NULL;
	struct rb_node *parent = NULL;
	struct client_block key;
	int cmp;

	key.bnr = bnr;
	clb_addr_from_sin(&key, addr);
	if (prev)
		*prev = NULL;
	if (next)
		*next = NULL;

	while (*link) {
		clb = container_of(*link, struct client_block, node);
		parent = *link;

		cmp = cmp_client_blocks(&key, clb);
		if (cmp < 0) {
			link = &(*link)->rb_left;
			if (next)
				*next = clb;
		} else if (cmp > 0) {
			link = &(*link)->rb_right;
			if (prev)
				*prev = clb;
		} else {
			break;
		}
		clb = NULL;
	}

	if (!clb && alloc) {
		clb = alloc_client_block(addr, bnr);
		if (!IS_ERR(clb))
			insert_client_block(inst, clb, parent, link);
	}

	return clb;
}

static void try_free_null_client_block(struct cache_mode_instance *inst, struct client_block *clb)
{
	if (clb->request == RPDFS_CACHE_MODE_NULL && clb->grant == RPDFS_CACHE_MODE_NULL &&
	    clb->revoke == RPDFS_CACHE_MODE_NULL) {
		if (only_client_with_block(clb))
			free_map_add_cached(clb->bnr, -1);
		rb_erase(&clb->node, &inst->client_blocks_root);
		list_del_init(&clb->lru_head);
		inst->nr_blocks--;
		dtracef("cache_mode_free", "clb %p "CLB_FMT, clb, CLB_ARG(clb));
		free(clb);
	}
}

static void lru_accessed(struct cache_mode_instance *inst, struct client_block *clb)
{
	list_move_tail(&clb->lru_head, &inst->lru_list);
}

static struct client_block *first_client_block(struct cache_mode_instance *inst, u64 bnr)
{
	struct sockaddr_in addr = {0,};
	struct client_block *clb;

	/* can never have a client block with 0 addr, can only get next */
	search_client_blocks(inst, &addr, bnr, false, NULL, &clb);
	if (clb && clb->bnr != bnr)
		clb = NULL;
	return clb;
}

static struct client_block *next_client_block(struct client_block *clb, u64 bnr)
{
	clb = next_clb(clb);
	if (clb && clb->bnr != bnr)
		clb = NULL;
	return clb;
}

#define for_each_client_block(inst_, bnr_, clb_) \
	for (clb_ = first_client_block(inst_, bnr_); clb_; clb_ = next_client_block(clb_, bnr_))

/*
 * We randomly choose which request to process amongst the waiting
 * requests.  We then mark it as being processed so we'll process it
 * fully before moving on to the next.  We won't process a request from
 * a client for a block until all the sent revokes have been confirmed.
 */
static struct client_block *find_request(struct cache_mode_instance *inst, u64 bnr)
{
	struct client_block *next;
	struct client_block *clb;
	u32 greatest;
	u32 hash;
	u32 seed;

	get_random(&seed, sizeof(seed));
	greatest = 0;
	next = NULL;

	for_each_client_block(inst, bnr, clb) {
		if (clb->request < RPDFS_CACHE_MODE_READ || clb->revoke)
			continue;

		if (clb->processing)
			goto out;

		hash = jhash_2words(clb->ipv4_addr, clb->ipv4_port, seed);
		if (next == NULL || hash > greatest) {
			next = clb;
			greatest = hash;
		}
	}

	if (next) {
		clb = next;
		clb->processing = 1;
	} else {
		clb = NULL;
	}
out:
	return clb;
}

static int send_read_result(struct cache_mode_instance *inst, struct client_block *clb,
			    u8 mode, struct cached_block *cblk, struct rpdfs_block_details *det,
			    int read_ret)
{
	struct rpdfs_msg_block_read_result rr;
	struct rpdfs_msg_header hdr;
	struct sockaddr_in addr;
	struct page *data_page;

	lru_accessed(inst, clb);

	memset_zero_sizeof(rr);
	rr.bnr = cpu_to_le64(clb->bnr);
	rr.grant_mode = mode;
	rr.err = rpdfs_msg_err(read_ret);

	hdr.ctl_size = sizeof(rr);
	hdr.data_size = 0;
	hdr.type = RPDFS_MSG_BLOCK_READ_RESULT;
	data_page = NULL;

	if (read_ret == 0) {
		rr.det.alloc_ctr = det->alloc_ctr;
		rr.det.wcount = det->write_ctr;
		rr.det.place_lo = det->place_lo;
		rr.det.place_hi = det->place_hi;
		if (clb->read_data) {
			hdr.data_size = cpu_to_le16(RPDFS_BLOCK_SIZE);
			data_page = block_data_page(cblk);
		}
	}

	clb_addr_to_sin(&addr, clb);
	return net_send(&addr, &hdr, &rr, data_page);
}

static int send_cache_mode(struct cache_mode_instance *inst, struct client_block *clb,
			   u8 type, u8 mode)
{
	struct rpdfs_msg_cache_mode cm;
	struct rpdfs_msg_header hdr;
	struct sockaddr_in addr;

	lru_accessed(inst, clb);

	cm.bnr = cpu_to_le64(clb->bnr);
	cm.mode = mode;
	memset_zero_sizeof(cm._pad);

	hdr.data_size = 0;
	hdr.ctl_size = sizeof(cm);
	hdr.type = type;

	clb_addr_to_sin(&addr, clb);
	return net_send(&addr, &hdr, &cm, NULL);
}

/*
 * Advance the state machine across all the clients for a given block.
 * The driver of change is incoming requests.  If their requested mode
 * is incompatible with other granted modes then we send revokes.  Once
 * the revokes are confirmed then the request is compatible with other
 * grants and we can send a grant for the request.
 *
 * This takes responsibility for sending a read result along with its
 * data contents on behalf of a read request.  This can happen long
 * after the initial read request was recorded and after having sent a
 * revocations and received confirmations.
 */
static void process_requests(struct cache_mode_instance *inst, u64 bnr)
{
	struct cached_block *cblk = NULL;
	struct rpdfs_block_details det;
	struct client_block *req;
	struct client_block *clb;
	bool saw_incompat;
	int read_ret = 0;
	u8 grant_mode;
	u8 compat;
	int ret;

restart:
	while ((req = find_request(inst, bnr))) {

		dtracef("cache_mode_process", "clb "CLB_FMT, CLB_ARG(req));
		compat = most_compatible(req->request);
		saw_incompat = false;

		for_each_client_block(inst, bnr, clb) {
			if (clb == req || compatible_modes(clb->grant, req->request))
				continue;

			saw_incompat = true;

			if (!clb->revoke) {
				ret = send_cache_mode(inst, clb, RPDFS_MSG_BLOCK_REVOKE_MODE,
						      compat);
				BUG_ON(ret < 0); /* evict?  shutdown? */

				clb->revoke = compat;
				if (clb->revoke == RPDFS_CACHE_MODE_NONE)
					inst->nr_revoke_none++;
				dtracef("cache_mode_revoke", "clb "CLB_FMT, CLB_ARG(clb));
			}
		}

		if (saw_incompat)
			break;

		/* other utasks can finish reads and respond before we wake, always restart */
		if (req->is_read && cblk == NULL && read_ret == 0) {
			read_ret = bstore_read(bnr, &cblk, &det);
			goto restart;
		}

		/* requests can cross with free_stripe_grants, don't grant less */
		if (req->request > req->grant)
			grant_mode = req->request;
		else
			grant_mode = req->grant;

		if (req->is_read)
			ret = send_read_result(inst, req, grant_mode, cblk, &det, read_ret);
		else
			ret = send_cache_mode(inst, req, RPDFS_MSG_BLOCK_GRANT_MODE, grant_mode);
		BUG_ON(ret < 0); /* evict?  shutdown? */

		req->grant = grant_mode;
		req->request = RPDFS_CACHE_MODE_NULL;
		req->is_read = 0;
		req->processing = 0;
		req->read_data = 0;
		dtracef("cache_mode_grant", "clb "CLB_FMT, CLB_ARG(req));
	}

	block_put(cblk);
}

static void try_shrink_lru(struct cache_mode_instance *inst)
{
	struct client_block *clb;
	struct client_block *_clb_;
	int ret;

	list_for_each_entry_safe(clb, _clb_, &inst->lru_list, lru_head) {
		if (inst->nr_blocks - inst->nr_revoke_none <= inst->block_limit)
			break;

		/* only shrink idle blocks */
		if (!(clb->request || clb->revoke)) {
			ret = send_cache_mode(inst, clb, RPDFS_MSG_BLOCK_REVOKE_MODE,
					      RPDFS_CACHE_MODE_NONE);
			BUG_ON(ret < 0); /* evict?  shutdown? */

			clb->revoke = RPDFS_CACHE_MODE_NONE;
			inst->nr_revoke_none++;
			dtracef("cache_mode_shrink", "clb "CLB_FMT, CLB_ARG(clb));
		}

		list_move_tail(&clb->lru_head, &inst->lru_list);
	}
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
	struct client_block *clb;
	int ret;

	/* only support requesting elevated modes for now */
	if (mode < RPDFS_CACHE_MODE_READ) {
		ret = -EINVAL;
		goto out;
	}

	clb = search_client_blocks(inst, addr, bnr, true, NULL, NULL);
	if (IS_ERR(clb)) {
		ret = PTR_ERR(clb);
		goto out;
	}

	/* shouldn't request dupes, or read while write in flight */
	if (mode <= clb->request) {
		ret = -EINVAL;
		goto out;
	}

	clb->request = mode;
	if (is_read && !clb->is_read) {
		clb->is_read = 1;
		clb->read_data = !!with_data;
	}

	dtracef("cache_mode_request", "clb "CLB_FMT, CLB_ARG(clb));
	ret = 0;
out:
	process_requests(inst, bnr);
	try_shrink_lru(inst);
	dtracef("cache_mode_request_ret", "addr "IPV4F" bnr %llu mode %u ret %d",
		IPV4A(addr), bnr, mode, ret);
	return ret;
}

/*
 * We must receive one confirm matching the mode of each revoke we send.
 * We'll only ever have one revoke message in flight waiting for a
 * confirm.
 */
int cache_mode_confirm(struct sockaddr_in *addr, u64 bnr, u8 mode)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct client_block *clb = NULL;
	int ret;

	if (mode < RPDFS_CACHE_MODE_NONE || mode > RPDFS_CACHE_MODE_READ) {
		ret = -EPROTO;
		goto out;
	}

	clb = search_client_blocks(inst, addr, bnr, false, NULL, NULL);
	if (!clb || mode != clb->revoke) {
		ret = -EPROTO;
		goto out;
	}

	if (mode > RPDFS_CACHE_MODE_NONE)
		clb->grant = mode;
	else
		clb->grant = RPDFS_CACHE_MODE_NULL;
	if (clb->revoke == RPDFS_CACHE_MODE_NONE)
		inst->nr_revoke_none--;
	clb->revoke = RPDFS_CACHE_MODE_NULL;

	dtracef("cache_mode_confirm", "clb "CLB_FMT, CLB_ARG(clb));
	try_free_null_client_block(inst, clb);

	ret = 0;
out:
	process_requests(inst, bnr);
	try_shrink_lru(inst);
	dtracef("cache_mode_confirm_ret", "addr "IPV4F" bnr %llu mode %u ret %d",
		IPV4A(addr), bnr, mode, ret);
	return ret;
}

/*
 * For each block, grant the client the given mode if the block is not
 * cached by any clients.
 *
 * The caller's bitmap defines the blocks to try and grant, relative to
 * the bmap_bnr.  If a requested block is already cached we clear its
 * bit in the caller's bmap.  We return the number of blocks granted
 * (matching the remaining number of set bits on return).
 *
 * We only modify the in-memory tracking of the grants.  The caller is
 * sending a message that communicates the grants.  If they can't send
 * the grants then they call back in to undo the grants.
 */
int cache_mode_grant_bulk_uncached(struct sockaddr_in *addr, int mode, u64 bmap_bnr,
				   unsigned long *bmap, size_t size)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	static struct sockaddr_in zero_addr = {0, };
	struct client_block *found;
	struct client_block *prev;
	struct client_block *next;
	struct client_block *ins;
	size_t undo_size = 0;
	unsigned long b;
	int count = 0;
	u64 dist;
	u64 bnr;
	int ret;

	/* so we don't accidentally grant null/none, wouldn't make sense */
	if (WARN_ON_ONCE(mode < RPDFS_CACHE_MODE_READ))
		return -EINVAL;

	dist = bstore_contig_devd_block_bnr_distance();

	b = find_next_bit(bmap, size, 0);
	bnr = bmap_bnr + (b * dist);
	found = search_client_blocks(inst, &zero_addr, bnr, false, &prev, &next);
	if (found)
		next = found;

	while (b < size) {
		/* grant mode for uncached set bits */
		while ((b < size) && (!prev || (bnr > prev->bnr)) && (!next || (bnr < next->bnr))) {
			ins = alloc_client_block(addr, bnr);
			if (IS_ERR(ins)) {
				ret = PTR_ERR(ins);
				goto out;
			}

			ins->grant = mode;
			dtracef("cache_mode_grant_bulk", "clb "CLB_FMT, CLB_ARG(ins));

			insert_between(inst, prev, ins, next);
			prev = ins;
			undo_size = b + 1;
			count++;

			b = find_next_bit(bmap, size, b + 1);
			bnr = bmap_bnr + (b * dist);
		}

		/* clear cached bits */
		if ((b < size) && ((prev && (bnr == prev->bnr)) || (next && (bnr == next->bnr)))) {
			clear_bit(b, bmap);
			b = find_next_bit(bmap, size, b + 1);
			bnr = bmap_bnr + (b * dist);
		}

		/* skip client blocks until we could ins or clear the next set bit */
		while (next && bnr > next->bnr) {
			prev = next;
			next = next_clb(next);
		}
	}

	ret = 0;
out:
	if (ret < 0 && undo_size > 0)
		cache_mode_undo_bulk_grant(addr, bmap_bnr, bmap, undo_size);

	try_shrink_lru(inst);

	return ret ?: count;
}

/*
 * This slow error path undoes the grants that were just created by
 * _grant_bulk_uncached.  Nothing else should have happened since.  We
 * must find the granted blocks and they should only have their grant
 * mode set.
 */
void cache_mode_undo_bulk_grant(struct sockaddr_in *addr, u64 bnr,
				unsigned long *bmap, size_t size)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct client_block *clb;
	unsigned long b;
	u64 dist;

	dist = bstore_contig_devd_block_bnr_distance();

	for (b = 0; (b = find_next_bit(bmap, size, b)) < size; b++) {
		clb = search_client_blocks(inst, addr, bnr + (b * dist), false, NULL, NULL);
		BUG_ON(IS_ERR_OR_NULL(clb));

		clb->grant = RPDFS_CACHE_MODE_NULL;
		dtracef("cache_mode_undo_bulk", "clb "CLB_FMT, CLB_ARG(clb));
		try_free_null_client_block(inst, clb);
	}
}

void cache_mode_accessed(struct sockaddr_in *addr, u64 bnr)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct client_block *clb;

	clb = search_client_blocks(inst, addr, bnr, false, NULL, NULL);
	if (!IS_ERR_OR_NULL(clb))
		lru_accessed(inst, clb);
}

int cache_mode_init(void)
{
	return 0;
}

void cache_mode_exit(void)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct client_block *clb;
	struct client_block *n;

	rbtree_postorder_for_each_entry_safe(clb, n, &inst->client_blocks_root, node)
		free(clb);

	inst->nr_blocks = 0;
	inst->nr_revoke_none = 0;
	inst->client_blocks_root = RB_ROOT;
}
