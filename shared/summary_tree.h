/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_SUMMARY_TREE_H
#define NGNFS_SHARED_SUMMARY_TREE_H

#include "shared/lk/types.h"

struct summary_tree;

typedef u64 (*smtree_summarize_fn)(u64 *words, unsigned short nr);
typedef unsigned short (*smtree_search_fn)(u64 *words, unsigned short nr);

void smtree_set(struct summary_tree *smt, smtree_summarize_fn fn, unsigned long pos, u64 word);
unsigned long smtree_search(struct summary_tree *smt, smtree_search_fn fn);
struct summary_tree *smtree_alloc(unsigned short fanout, unsigned long nr_leaves);

#endif
