/* SPDX-License-Identifier: GPL-2.0 */

#include <stdlib.h>

#include "shared/lk/bug.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"
#include "shared/lk/overflow.h"
#include "shared/lk/stddef.h"

#include "shared/summary_tree.h"

/*
 * On disk, summary blocks contain dense arrays of words that each
 * summarize a block's worth of details for each stored block.
 *
 * These in-memory trees continue those summary words up into higher
 * levels.  The tree is loaded at device startup and then incrementally
 * updated as blocks change.  It avoids the write amplification of
 * maintaining the relatively small higher level summaries on disk.
 *
 * Searches for a block with a given property are accelerated by
 * searching through levels of the in-memory summary tree words, then a
 * persistent summary block, and finally a details block.
 *
 * The in-memory summaries are fixed-size.  We allocate one array and
 * use implicit addressing to derive the array index of parents and
 * children of nodes.
 */

struct summary_tree {
	unsigned long nr_leaves;
	unsigned long total;
	unsigned short fanout;
	u64 words[0];
};

typedef u64 (*smtree_summarize_fn)(u64 *words, unsigned short nr);
typedef unsigned short (*smtree_search_fn)(u64 *words, unsigned short nr);

/*
 * (say a fanout of 8)
 *
 * 0..7
 * 8..15 16..23 [...] 64..71
 * 72..79
 */

static inline unsigned long has_parent(struct summary_tree *smt, unsigned long ind)
{
	return ind >= smt->fanout;
}

/*
 * Returns the index of the word that is the parent for the given index.
 */
static inline unsigned long parent_ind(struct summary_tree *smt, unsigned long ind)
{
	return (ind / smt->fanout) - 1;
}

/*
 * Returns the index of the first fanout nr of children of the given ith
 * word relative to the ind parent.
 */
static inline unsigned long child_ind(struct summary_tree *smt, unsigned long ind,
				      unsigned short i)
{
	return (ind + i + 1) * smt->fanout;
}

static inline unsigned long leaf_ind(struct summary_tree *smt, unsigned long pos)
{
	return smt->total - smt->nr_leaves + pos;
}

void smtree_set(struct summary_tree *smt, smtree_summarize_fn fn, unsigned long pos, u64 word)
{
	unsigned long ind;

	BUG_ON(pos >= smt->nr_leaves);

	ind = leaf_ind(smt, pos);
	for (;;) {
		smt->words[ind] = word;
		if (!has_parent(smt, ind))
			break;

		word = fn(smt->words + rounddown(ind, smt->fanout), smt->fanout);
		ind = parent_ind(smt, ind);
	}
}

/*
 * Returns the leaf position from 0 to (nr_leaves - 1) if the search
 * succeeds.  Returns nr_leaves if the search failed.
 */
unsigned long smtree_search(struct summary_tree *smt, smtree_search_fn fn)
{
	unsigned long ind = 0;
	unsigned long leaf;
	unsigned short nr;
	unsigned short i;

	for (;;) {
		nr = min(smt->fanout, smt->total - ind);
		i = fn(&smt->words[ind], nr);
		if (i >= nr)
			return smt->total;

		ind = child_ind(smt, ind, i);

		/*
		 * This shouldn't happen.  We shouldn't set words in
		 * parents that lead to leaves that don't exist.  But
		 * we'll be careful.
		 */
		if (WARN_ON_ONCE(ind >= smt->total))
			return smt->total;

		leaf = leaf_ind(smt, 0);
		if (ind >= leaf)
			return ind - leaf;
	}
}

/*
 * Allocate the array for the words in the summary tree.  We always have
 * a first level with [fanout] words.  We multiply the number of leaves
 * in the last level by the fanout until we have sufficient leaves to
 * store the caller's leaves.  Summarizations of words at a level to
 * store int he parent is always done in groups of [fanout] so the final
 * number of leaves isn't truncated to the caller's nr.
 *
 * The returned pointer can be freed with free().
 */
struct summary_tree *smtree_alloc(unsigned short fanout, unsigned long nr_leaves)
{
	struct summary_tree *smt;
	unsigned long total;
	unsigned long leaves;
	size_t bytes;

	total = fanout;
	leaves = fanout;
	while (leaves < nr_leaves) {
		if (check_mul_overflow(leaves, fanout, &leaves) ||
		    check_add_overflow(total, leaves, &total))
			return NULL;
	}

	if (check_mul_overflow(total, sizeof_field(struct summary_tree, words[0]), &bytes) ||
	    check_add_overflow(bytes, sizeof(struct summary_tree), &bytes))
		return NULL;

	smt = malloc(bytes);
	if (smt) {
		memset(smt, 0, bytes);
		smt->nr_leaves = nr_leaves;
		smt->total = total;
		smt->fanout = fanout;
	}

	return smt;
}
