/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_CLIST_H
#define RPDFS_SHARED_CLIST_H

/*
 * I could have sworn that I saw something like this upstream once,
 * somewhere..
 */
struct counted_list_head {
	struct list_head head;
	unsigned long count;
};

static inline void clist_del_init(struct list_head *head, struct counted_list_head *clist)
{
	list_del_init(head);
	clist->count--;
}

static inline void clist_add_tail(struct list_head *head, struct counted_list_head *clist)
{
	list_add_tail(head, &clist->head);
	clist->count++;
}

#endif
