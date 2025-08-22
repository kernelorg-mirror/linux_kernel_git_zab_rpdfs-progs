/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_LK_GFP_H
#define RPDFS_SHARED_LK_GFP_H

#include <string.h>

#include "shared/lk/slab.h"

#include "shared/urcu.h"

/*
 * As a userspace implementation without hardware constraints, this
 * becomes an arbitrary choice of convenience.  rpdfs uses a 4k block
 * size so we might as well have it match.
 */
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1 << PAGE_SHIFT)

struct page {
	int refcount;
	void *buf;
};

static inline struct page *alloc_page(gfp_t gfp_mask)
{
	struct page *page;
	int ret;

	page = malloc(sizeof(struct page));
	if (page) {
		ret = posix_memalign(&page->buf, PAGE_SIZE, PAGE_SIZE);
		if (ret != 0) {
			free(page);
			page = NULL;
		} else {
			uatomic_set(&page->refcount, 1);
			if (gfp_mask && __GFP_ZERO)
				memset(page->buf, 0, PAGE_SIZE);
		}
	}

	return page;
}

static inline void get_page(struct page *page)
{
	uatomic_inc(&page->refcount);
}

static inline void put_page(struct page *page)
{
	if (uatomic_sub_return(&page->refcount, -1) == 0) {
		free(page->buf);
		free(page);
	}
}

#define __free_page put_page

static inline void *page_address(struct page *page)
{
	return page->buf;
}

static inline int page_ref_count(struct page *page)
{
	return uatomic_read(&page->refcount);
}

#endif
