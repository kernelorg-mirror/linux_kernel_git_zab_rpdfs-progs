/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_SHARED_HASH_TABLE_H
#define RPDFS_SHARED_HASH_TABLE_H

struct hash_table;

struct hash_table *htable_alloc(unsigned long limit);
void htable_insert(struct hash_table *ht, u64 key, u64 val);
u64 htable_lookup(struct hash_table *ht, u64 key);
void htable_foreach_init(struct hash_table *ht, unsigned long *fe);
u64 htable_foreach(struct hash_table *ht, unsigned long *fe);
void htable_delete(struct hash_table *ht, u64 key);
void htable_clear(struct hash_table *ht);

#endif
