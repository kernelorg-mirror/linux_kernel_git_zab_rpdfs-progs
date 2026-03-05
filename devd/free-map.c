/* SPDX-License-Identifier: GPL-2.0 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include "shared/lk/bug.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/log2.h"
#include "shared/lk/math.h"
#include "shared/lk/types.h"

#include "shared/dtracef.h"

#include "devd/free-map.h"

/*
 * This tracks how many uncached free blocks are found in the stripes
 * that could be sent in response to a searching free_stripe request.
 *
 * First we have an array of counts of both free and cached blocks per
 * stripe.  Then we add a tournament tree on top that tracks the
 * greatest (free - cached) difference and the index into the array for
 * each stripe.
 *
 * In O(1) we can look at the root element of the tournament tree to
 * find the stripe with the most uncached free blocks.
 *
 * As bstore modifies details blocks it updates a stripe's free count
 * and as cache-mode tracks blocks it updates the cached count.  Most of
 * the time the stripes won't be the ones we can return for searches so
 * these updates are nops.  When we do need to update the tournament
 * tree we often only update a few lower level entries, which is a few
 * cache misses and stores.
 */

/*
 * The funny bitfield lets us not waste memory on padding while having
 * an index that doesn't result in an uncomfortably small max device
 * size, given device capacities at the time of writing.
 */
#define DIFF_BITS	8
#define IND_BITS	(64 - DIFF_BITS)
static struct free_map_instance {
	u64 stripe_size;
	u64 nr_stripes;
	u64 my_stripe;
	unsigned long nr_tourn;
	unsigned long nr_counts;
	struct tourn_entry {
		s64 ind:IND_BITS,
		    diff:DIFF_BITS;
	} *tourn;
	struct stripe_counts {
		s8 free;
		s8 cached;
	} *counts;
} global_free_map_inst = {
};

static bool is_search_stripe(struct free_map_instance *inst, u64 bnr, unsigned long *ind)
{
	*ind = bnr / (inst->stripe_size * inst->nr_stripes);

	return (*ind % inst->nr_stripes) == inst->my_stripe;
}

static inline s8 counts_diff(struct free_map_instance *inst, unsigned long ind)
{
	return inst->counts[ind].free - inst->counts[ind].cached;
}

/*
 * The caller has changed the leaf counts for their index.  We update
 * our tournament array tracking of the first most free.  That is, the
 * lowest index of those with the greatest (free - cached) difference.
 *
 * The core loop figures out which of a pair of child siblings is
 * greatest.  If the parent already matches that greater child then
 * we're done.  But if they differ then we update the parent and ascend,
 * with the next pair of children being the previous parent and its
 * sibling.
 *
 * We start it off by synthesizing child nodes from the pair of leaf
 * counts that contain the caller's update.
 */
static void update_tourn_parents(struct free_map_instance *inst, unsigned long ind)
{
	struct tourn_entry from_counts[2];
	struct tourn_entry *child;
	unsigned long parent;
	unsigned long right;

	from_counts[0].ind = ind & ~1UL;
	from_counts[0].diff = counts_diff(inst, from_counts[0].ind);
	from_counts[1].ind = ind | 1;
	from_counts[1].diff = counts_diff(inst, from_counts[1].ind);
	parent = inst->nr_tourn + ind;
	child = from_counts;

	do {
		parent = (parent - 1) / 2;
		right = !!((child[1].diff > child[0].diff) ||
			   (child[1].diff == child[0].diff && child[1].ind < child[0].ind));

		if (inst->tourn[parent].ind == child[right].ind &&
		    inst->tourn[parent].diff == child[right].diff)
			return;

		inst->tourn[parent] = child[right];
		child = &inst->tourn[(parent - 1) | 1];

	} while (parent > 0);

	if (parent == 0) {
		child = ind == from_counts[0].ind ? &from_counts[0] : &from_counts[1];
		dtracef("free_map_new_first_most",
			"tourn[0] = {%d, %llu} from count[%lu] = {%d, %llu}",
			inst->tourn[0].diff, (u64)inst->tourn[0].ind,
			ind, child[0].diff, (u64)child[0].ind);
	}
}

void free_map_set_free(u64 bnr, u8 free)
{
	struct free_map_instance *inst = &global_free_map_inst;
	unsigned long ind;

	if (!is_search_stripe(inst, bnr, &ind))
		return;

	if (inst->counts[ind].free != free) {
		dtracef("free_map_set_free", "ind %lu free %d", ind, free);
		inst->counts[ind].free = free;
		update_tourn_parents(inst, ind);
	}
}

void free_map_add_cached(u64 bnr, s8 delta)
{
	struct free_map_instance *inst = &global_free_map_inst;
	unsigned long ind;

	if (!is_search_stripe(inst, bnr, &ind) || delta == 0)
		return;

	inst->counts[ind].cached += delta;
	dtracef("free_map_add_cached", "ind %lu delta %d after %d",
		ind, delta, inst->counts[ind].cached);
	update_tourn_parents(inst, ind);
}

int free_map_find_first_most(u64 *bnr_ret)
{
	struct free_map_instance *inst = &global_free_map_inst;
	int ret;

	if (inst->tourn[0].diff <= 0) {
		ret = -ENOSPC;
		*bnr_ret = 0;
	} else {
		ret = 0;
		*bnr_ret = (inst->tourn[0].ind * inst->stripe_size * inst->nr_stripes) +
			   inst->my_stripe;
	}

	dtracef("free_map_find_first_most", "[0] ind %llu diff %d ret %d bnr %llu",
		(u64)inst->tourn[0].ind, inst->tourn[1].diff, ret, *bnr_ret);
	return ret;
}

/* a bit brute force, but functional */
static bool entry_bits_fit(s64 ind, s64 diff)
{
	struct tourn_entry dummy = {
		.ind = ind,
		.diff = diff,
	};

	return dummy.ind == ind && dummy.diff == diff;
}

/*
 * The free map geometry is static for now.  Eventually we'll associate
 * it with versions of quorum updates.
 */
int free_map_init(u64 blocks, u64 stripe_size, u64 nr_stripes, u64 my_stripe)
{
	struct free_map_instance *inst = &global_free_map_inst;
	int ret;

	if (WARN_ON_ONCE(!entry_bits_fit(blocks / (stripe_size * nr_stripes), 0)) ||
	    WARN_ON_ONCE(!entry_bits_fit(0, stripe_size)) ||
	    WARN_ON_ONCE(!entry_bits_fit(0, -stripe_size)))
		return -EINVAL;

	inst->stripe_size = stripe_size;
	inst->nr_stripes = nr_stripes;
	inst->my_stripe = my_stripe;
	inst->nr_counts = round_up(blocks / stripe_size, 2);
	inst->nr_tourn = roundup_pow_of_two(inst->nr_counts) - 1;

	inst->tourn = calloc(inst->nr_tourn, sizeof(inst->tourn[0]));
	inst->counts = calloc(inst->nr_counts, sizeof(inst->counts[0]));
	if (!inst->tourn || !inst->counts) {
		free(inst->tourn);
		free(inst->counts);
		memset(inst, 0, sizeof(struct free_map_instance));
		ret = -ENOMEM;
		goto out;
	}

	ret = 0;
out:
	return ret;
}

void free_map_exit(void)
{
	struct free_map_instance *inst = &global_free_map_inst;

	free(inst->tourn);
	free(inst->counts);
	memset(inst, 0, sizeof(struct free_map_instance));
}
