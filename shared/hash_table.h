/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_HASH_TABLE_H
#define RPDFS_SHARED_HASH_TABLE_H

struct hash_table;
typedef void (*htable_callback_t)(void *obj, void *arg);

struct hash_table *htable_alloc(size_t key_offset, size_t key_size);
void htable_insert(struct hash_table *ht, void *obj);
void *htable_lookup(struct hash_table *ht, void *key);
void htable_delete(struct hash_table *ht, void *key);
void htable_destroy(struct hash_table *ht, htable_callback_t cb, void *arg);

#endif
