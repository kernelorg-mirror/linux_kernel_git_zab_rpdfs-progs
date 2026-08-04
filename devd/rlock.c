/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "shared/lk/bitops-le.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/minmax.h"
#include "shared/compare.h"
#include "shared/dtracef.h"
#include "shared/format-block.h"
#include "shared/format-dev-log.h"
#include "shared/format-msg.h"
#include "shared/hash_table.h"
#include "shared/msg.h"
#include "shared/string_wrappers.h"
#include "utask/net.h"
#include "utask/utask.h"
#include "devd/rlock.h"

/*
 * This server processes rlock messages from clients.  Clients request
 * an rlock mode for a key which is generally the granularity of an
 * inode.  The server ensures that clients aren't able to use
 * conflicting modes for a given key(/inode).
 *
 * The clients control the life cycle of their rlock.  They eventually
 * release unused rlocks which allows the server to free its tracking.
 *
 * This requires in-order messaging to ensure that back-to-back sends
 * aren't processed out of order on the receiver.
 */

static struct rlock_instance {
	struct hash_table *lists_ht;
	struct hash_table *client_ht;
} global_cache_mode_inst;

struct rlock_request {
	struct rlock_request *next;
	u64 client_id;
	u8 mode;
};

/*
 * The lists of rlocks granted to clients or requested from clients for
 * a given key.
 */
struct rlock_lists {
	struct rpdfs_rlock_key key;
	struct list_head granted;
	struct list_head requested;
	struct rlock_request *head_req;
	struct rlock_request *tail_req;
};

/*
 * Tracks the modes in flight to a specific client.
 */
struct client_rlock {
	struct client_hash_key {
		struct rpdfs_rlock_key key;
		u64 client_id;
	} hk;
	struct list_head head;
	u8 granted;
	u8 revoked;
};

#define CL_KEY_FMT		"%llu.%llu"
#define CL_KEY_ARG(key)		le64_to_cpu((key)->k[0]), le64_to_cpu((key)->k[1])

#define CL_FMT			"key "CL_KEY_FMT" client_id %llu gr %u rvk %u"
#define CL_ARG(lists, cl)	CL_KEY_ARG(&(lists)->key), (cl)->hk.client_id, (cl)->granted, \
				(cl)->revoked

#define REQ_FMT			"key "CL_KEY_FMT" client_id %llu mode %u"
#define REQ_ARG(lists, req)	CL_KEY_ARG(&(lists)->key), (req)->client_id, (req)->mode


/*
 * Returns true if the two modes can both be granted to different
 * clients.  Only writes are incompatible with other reads or writes.
 * All other combinations, including writes with null or none, are
 * compatible.
 */
static unsigned int compatible_modes(u8 a, u8 b)
{
	if (a > b)
		swap(a, b);

	return b < RPDFS_RLOCK_MODE_EX_WR || a <= RPDFS_RLOCK_MODE_NONE;
}

/*
 * Returns the least restrictive mode that's compatible with the
 * requested mode.  Writes conflicting with reads are reduced to reads
 * to keep their read cache.  All modes conflicting with requested
 * writes needs to drop their cache so they can later read the result of
 * the write.
 */
static inline u8 most_compatible(u8 mode)
{
	return mode == RPDFS_RLOCK_MODE_EX_WR ? RPDFS_RLOCK_MODE_NONE : RPDFS_RLOCK_MODE_SH_RD;
}

/*
 * When the qlists are up they'll define unique 64bit client ids which
 * will become the endpoint specifier for the network API.  We're faking
 * it until then.
 */
static u64 sockaddr_to_client_id(struct sockaddr_in *addr)
{
	return ((u64)ntohl(addr->sin_addr.s_addr) << 32) | ntohs(addr->sin_port);
}

static void client_id_to_sockaddr(struct sockaddr_in *addr, u64 client_id)
{
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = htonl(client_id >> 32);
	addr->sin_port = htons(client_id & U16_MAX);
}

static struct rlock_lists *get_rlock_lists(struct rlock_instance *inst,
					   struct rpdfs_rlock_key *key, bool alloc)
{
	struct rlock_lists *lists = NULL;

	lists = htable_lookup(inst->lists_ht, key);
	if (!lists && alloc) {
		lists = malloc(sizeof(struct rlock_lists));
		if (lists) {
			lists->key = *key;
			INIT_LIST_HEAD(&lists->granted);
			lists->head_req = NULL;
			lists->tail_req = NULL;

			htable_insert(inst->lists_ht, lists);
		}
	}

	return lists;
}

static struct client_rlock *get_client_rlock(struct rlock_instance *inst,
					     struct rlock_lists *lists, u64 client_id,
					     struct rpdfs_rlock_key *key, bool alloc)
{
	struct client_rlock *cl = NULL;
	struct client_hash_key hk = {
		.key = *key,
		.client_id = client_id,
	};

	if (WARN_ON_ONCE(alloc && !lists))
		return NULL;

	cl = htable_lookup(inst->client_ht, &hk);
	if (!cl && alloc) {
		cl = malloc(sizeof(struct client_rlock));
		if (cl) {
			cl->hk.key = *key;
			cl->hk.client_id = client_id;
			cl->granted = RPDFS_RLOCK_MODE_NULL;
			cl->revoked = RPDFS_RLOCK_MODE_NULL;

			/* NONE/NULL sorted at the tail of the granted list */
			list_add_tail(&cl->head, &lists->granted);
			htable_insert(inst->client_ht, cl);
		}
	}

	return cl;
}

/*
 * Return the granted mode of a next/prev neighbouring client_rlock in
 * the granted list.  If the given cl doesn't have a neighbour then we
 * return its mode (so that the caller interprets it as being sorted.)
 */
static inline u8 nei_granted(struct rlock_lists *lists, struct client_rlock *cl, bool next)
{
	struct client_rlock *nei = cl;

	if (next && !list_is_last(&cl->head, &lists->granted))
		nei = list_next_entry(cl, head);
	else if (!next && !list_is_first(&cl->head, &lists->granted))
		nei = list_prev_entry(cl, head);
	return nei->granted;
}

/*
 * We keep the granted list sorted by decreasing mode so that we can
 * avoid walking all client grants when processing requests.  The single
 * EX_WR is first, then SH_RD, then NONE and NULL.
 *
 * We can repair the sort order incrementally as each individual
 * client's granted mode changes.  the action to take for each mode as
 * we're likely to add more modes We have sufficient list_heads in the
 * list to find the sorted position for each mode in O(1).
 *
 * This must be called when a granted mode is changed and before
 * changing another granted mode.  Only our caller's client mode can be
 * out of order.
 */
static void maintain_granted_order(struct rlock_lists *lists, struct client_rlock *cl)
{
	struct client_rlock *nei;

	if (cl->granted < nei_granted(lists, cl, true) ||
	    cl->granted > nei_granted(lists, cl, false)) {
		list_del_init(&cl->head);

		switch (cl->granted) {
		/* only one EX_WR, always first */
		case RPDFS_RLOCK_MODE_EX_WR:
			list_add(&cl->head, &lists->granted);
			break;
		/* all SH_RD after possible first EX_WR */
		case RPDFS_RLOCK_MODE_SH_RD:
			nei = list_first_entry(&lists->granted, struct client_rlock, head);
			if (nei->granted == RPDFS_RLOCK_MODE_EX_WR)
				list_add(&cl->head, &nei->head);
			else
				list_add(&cl->head, &lists->granted);
			break;
		/* all NONE/NULL in one final group */
		case RPDFS_RLOCK_MODE_NONE:
		case RPDFS_RLOCK_MODE_NULL:
			list_add_tail(&cl->head, &lists->granted);
			break;
		}
	}
}

static bool try_free_rlock_lists(struct rlock_instance *inst, struct rlock_lists *lists)
{
	if (list_empty(&lists->granted) && lists->head_req == NULL) {
		htable_delete(inst->lists_ht, &lists->key);
		free(lists);
		return true;
	}

	return false;
}

/*
 * We have no client state beyond the mode so we can free when they drop
 * under READ.  Eventually we'll have flags that are tracked with the
 * NONE mode and that will keep them from being freed.
 */
static bool try_free_client_rlock(struct rlock_instance *inst, struct rlock_lists *lists,
				  struct client_rlock *cl)
{
	if (cl->granted <= RPDFS_RLOCK_MODE_NONE && cl->revoked <= RPDFS_RLOCK_MODE_NONE) {
		htable_delete(inst->client_ht, &cl->hk);
		list_del_init(&cl->head);
		free(cl);
		return true;
	}

	return false;
}

static void append_request(struct rlock_lists *lists, struct rlock_request *req)
{
	req->next = NULL;

	if (!lists->head_req)
		lists->head_req = req;
	if (lists->tail_req)
		lists->tail_req->next = req;
	lists->tail_req = req;
}

static struct rlock_request *head_request(struct rlock_lists *lists)
{
	return lists->head_req;
}

static void free_head_request(struct rlock_lists *lists, struct rlock_request *req)
{
	BUG_ON(lists->head_req != req);

	lists->head_req = req->next;
	if (lists->tail_req == req)
		lists->tail_req = NULL;

	free(req);
}

static int send_rlock(struct rlock_instance *inst, struct rlock_lists *lists,
		       struct client_rlock *cl, u8 type, u8 mode)
{
	struct rpdfs_msg_header hdr;
	struct rpdfs_msg_rlock dl;
	struct sockaddr_in addr;

	dl.key = lists->key;
	dl.mode = mode;
	dl.flags = 0;
	memset_zero_sizeof(dl._pad);

	hdr.data_size = 0;
	hdr.ctl_size = sizeof(dl);
	hdr.type = type;

	client_id_to_sockaddr(&addr, cl->hk.client_id);
	return net_send(&addr, &hdr, &dl, NULL);
}

/*
 * Advance rlock state across all the clients for a given key.  The
 * driver of change is incoming requests.  If the requested mode is
 * incompatible with other granted modes then we send revokes.  Once the
 * revokes are confirmed then the request is compatible with the
 * remaining confirmed modes and we can send a grant for the request.
 */
static void process_requests(struct rlock_instance *inst, struct rlock_lists *lists)
{
	struct rlock_request *req;
	struct client_rlock *cl;
	bool saw_incompat;
	u8 compat;
	int ret;

	while ((req = head_request(lists))) {

		dtracef("rlock_process_request", REQ_FMT, REQ_ARG(lists, req));
		compat = most_compatible(req->mode);
		saw_incompat = false;

		list_for_each_entry(cl, &lists->granted, head) {
			dtracef("rlock_process_client", CL_FMT, CL_ARG(lists, cl));

			/* a client's mode can't conflict with itself */
			if (cl->hk.client_id == req->client_id)
				continue;

			/* once we're compatible all remaining sorted granted will be as well */
			if (compatible_modes(cl->granted, compat))
				break;

			saw_incompat = true;

			/* only need to send revocations once per request */
			if (cl->revoked == compat)
				break;

			ret = send_rlock(inst, lists, cl, RPDFS_MSG_RLOCK_REVOKE, compat);
			BUG_ON(ret < 0); /* XXX mark as revoked/errored, process after evicted */
			cl->revoked = compat;
		}

		if (saw_incompat)
			break;

		cl = get_client_rlock(inst, lists, req->client_id, &lists->key, true);
		BUG_ON(!cl); /* shutdown / self-eviction */

		if (cl->granted != req->mode) {
			ret = send_rlock(inst, lists, cl, RPDFS_MSG_RLOCK_GRANT, req->mode);
			BUG_ON(ret < 0); /* evict us/client */

			cl->granted = req->mode;
			dtracef("rlock_process_grant", CL_FMT, CL_ARG(lists, cl));
			maintain_granted_order(lists, cl);
		}

		free_head_request(lists, req);
	}
}

/*
 * The caller has modified a client's rlock.  We might be able to free
 * it and then also the lists if it was the last client we were
 * tracking.
 *
 * If we couldn't free the client then make sure it's still in sorted
 * granted order.  If we didn't free the lists then process requests.
 */
static void try_free_or_process(struct rlock_instance *inst, struct rlock_lists *lists,
				struct client_rlock *cl)
{
	if (!try_free_client_rlock(inst, lists, cl))
		maintain_granted_order(lists, cl);
	else if (try_free_rlock_lists(inst, lists))
		lists = NULL;

	if (lists)
		process_requests(inst, lists);
}

/*
 * Record a client's request for a mode.  Requests only elevate the mode
 * and the client won't release the rlock until its request is served so
 * that we don't have grants and releases in flight.  The client can
 * send concurrent requests for increasing modes and we'll always send a
 * grant for every request received.
 */
int rlock_request(struct sockaddr_in *addr, struct rpdfs_rlock_key *key, u8 mode)
{
	struct rlock_instance *inst = &global_cache_mode_inst;
	u64 client_id = sockaddr_to_client_id(addr);
	struct rlock_lists *lists = NULL;
	struct rlock_request *req;
	int ret;

	/* only support requesting elevated modes for now */
	if (mode < RPDFS_RLOCK_MODE_SH_RD) {
		ret = -EINVAL;
		goto out;
	}

	lists = get_rlock_lists(inst, key, true);
	if (!lists) {
		ret = -ENOMEM;
		goto out;
	}

	req = malloc(sizeof(struct rlock_request));
	if (!req) {
		ret = -ENOMEM;
		goto out;
	}

	req->client_id = client_id;
	req->mode = mode;
	append_request(lists, req);

	dtracef("rlock_request", REQ_FMT, REQ_ARG(lists, req));
	ret = 0;
out:
	process_requests(inst, lists);
	dtracef("rlock_request_ret", "addr "IPV4F" key "CL_KEY_FMT" mode %u ret %d",
		IPV4A(addr), CL_KEY_ARG(key), mode, ret);
	return ret;
}

/*
 * We must receive one confirm matching the mode of each revoke we send.
 * We only process one requested mode at a time so we'll only have one
 * revoke in flight to each client at a time.  The client can send a
 * release as we're sending a revoke, so their confirm response can be
 * for a NULL mode when they don't have the rlock anymore.
 */
int rlock_confirm(struct sockaddr_in *addr, struct rpdfs_rlock_key *key, u8 mode)
{
	struct rlock_instance *inst = &global_cache_mode_inst;
	u64 client_id = sockaddr_to_client_id(addr);
	struct rlock_lists *lists = NULL;
	struct client_rlock *cl = NULL;
	int ret;

	/* we'll never send a revoke that gives a write mode */
	if (mode >= RPDFS_RLOCK_MODE_EX_WR) {
		ret = -EPROTO;
		goto out;
	}

	/* won't free while revoke is pending, client can send nonsense */
	cl = get_client_rlock(inst, NULL, client_id, key, false);
	if (!cl) {
		ret = -EPROTO;
		goto out;
	}

	lists = get_rlock_lists(inst, key, false);
	BUG_ON(!lists); /* lists must exist when cl does */

	/* mode can be less if they released as we revoked, this catches no pending revoke */
	if (mode > cl->revoked) {
		ret = -EPROTO;
		goto out;
	}

	cl->granted = cl->revoked;
	cl->revoked = RPDFS_RLOCK_MODE_NULL;

	dtracef("rlock_confirm", CL_FMT, CL_ARG(lists, cl));

	try_free_or_process(inst, lists, cl);
	ret = 0;
out:
	dtracef("rlock_confirm_ret", "addr "IPV4F" key "CL_KEY_FMT" mode %u ret %d",
		IPV4A(addr), CL_KEY_ARG(key), mode, ret);
	return ret;
}

/*
 * The client can voluntarily release a rlock.  Typically in response to
 * memory pressure as unused rlocks build up.  If the client's released
 * mode is null then they've freed the rlock and we can do the same.  If
 * we have a revoke in flight then we'll wait for the confirm (which
 * will also have a null mode when they no longer have the rlock).
 */
int rlock_release(struct sockaddr_in *addr, struct rpdfs_rlock_key *key, u8 mode)
{
	struct rlock_instance *inst = &global_cache_mode_inst;
	u64 client_id = sockaddr_to_client_id(addr);
	struct rlock_lists *lists = NULL;
	struct client_rlock *cl = NULL;
	int ret;

	/* can't leave the max mode when releasing */
	if (mode >= RPDFS_RLOCK_MODE_EX_WR) {
		ret = -EPROTO;
		goto out;
	}

	/* we must have the client's rlock, client can send nonsense */
	cl = get_client_rlock(inst, NULL, client_id, key, false);
	if (!cl) {
		ret = -EPROTO;
		goto out;
	}

	lists = get_rlock_lists(inst, key, false);
	BUG_ON(!lists); /* lists must exist when cl does */

	/* release must be less than they were granted */
	if (mode >= cl->granted) {
		ret = -EPROTO;
		goto out;
	}

	/* we know they're not using the greater granted mode anymore */
	cl->granted = mode;

	dtracef("rlock_release", CL_FMT, CL_ARG(lists, cl));

	try_free_or_process(inst, lists, cl);
	ret = 0;
out:
	dtracef("rlock_release_ret", "addr "IPV4F" key "CL_KEY_FMT" mode %u ret %d",
		IPV4A(addr), CL_KEY_ARG(key), mode, ret);
	return ret;
}

static void free_htable_obj(void *obj, void *arg)
{
	free(obj);
}

int rlock_init(void)
{
	struct rlock_instance *inst = &global_cache_mode_inst;
	int ret;

	inst->lists_ht = htable_alloc(offsetof(struct rlock_lists, key),
				      sizeof(struct rpdfs_block_key));
	inst->client_ht = htable_alloc(offsetof(struct client_rlock, hk),
				       sizeof(struct client_hash_key));
	if (!inst->lists_ht || !inst->client_ht) {
		htable_destroy(inst->lists_ht, free_htable_obj, NULL);
		htable_destroy(inst->client_ht, free_htable_obj, NULL);
		ret = -ENOMEM;
	} else {
		ret = 0;
	}

	return ret;
}

void rlock_exit(void)
{
	struct rlock_instance *inst = &global_cache_mode_inst;

	htable_destroy(inst->lists_ht, free_htable_obj, NULL);
	htable_destroy(inst->client_ht, free_htable_obj, NULL);
}
