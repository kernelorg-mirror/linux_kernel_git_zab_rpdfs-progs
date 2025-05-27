/* SPDX-License-Identifier: GPL-2.0 */

#include <string.h>
#include <limits.h>

#include "shared/lk/build_bug.h"
#include "shared/lk/bug.h"
#include "shared/lk/limits.h"
#include "shared/lk/minmax.h"
#include "shared/lk/overflow.h"
#include "shared/lk/types.h"
#include "shared/lk/xxhash.h"

#include "shared/get_random.h"
#include "shared/hash_table.h"

/*
 * A single-threaded fixed size hash table that uses robin hood hashing.
 *
 * We forbid use of high bits of the key and reserve them for storing
 * the distance.  We use C bitfields to let the compiler deal with the
 * shifting and masking.
 */

/*
 * The longest expected probe sequence in robin hood grows slowly so we
 * can have a reasonably high load.
 */
#define LOAD_PCT 85

/*
 * The larger distance we can store, the more contiguous entries can be
 * populated before it overflows and we explode.
 *
 * On x86, when we make distance 8 bits, gcc always uses byte extension
 * moves to work with the dist bitfield.  If the key is in the low bits
 * then it stores an enormous mask in a register to mask away the high
 * dist bits.  We get fewer instructions and registers used if we put
 * the key in the high bits so it uses shifts with small immediate
 * counts to move the key into and out of its higher location in the
 * word.
 */
#define DIST_BITS	8
#define DIST_MAX	((1 << DIST_BITS) - 1)
#define KEY_BITS	(64 - DIST_BITS)
#define KEY_MAX		(U64_MAX >> DIST_BITS)
struct hash_table {
	u64 seed;
	unsigned long count;
	struct hash_table_entry {
		u64 dist:DIST_BITS,
		    key:KEY_BITS;
		u64 val;
	} entries[0];
};

static void build_assertions(void)
{
	/* don't have a silly sparse table, and we can spin forever if it's full */
	BUILD_BUG_ON(LOAD_PCT < 50 || LOAD_PCT >= 99);
	BUILD_BUG_ON(DIST_MAX > ULONG_MAX);
}

/*
 * Allocate a hash table that will fit up to the callers limit.  The
 * resulting pointer can be freed with free().
 *
 * (limit / alloc) = (LOAD_PCT / 100)
 * alloc = (limit * 100) / LOAD_PCT
 */
struct hash_table *htable_alloc(unsigned long limit)
{
	struct hash_table *ht;
	unsigned long count;
	size_t bytes;

	build_assertions();

	/* make sure all whole load pcts add at least one empty entry */
	limit = max(100, limit);

	/* bytes = offsetof(struct hash_table, entries[(limit * 100) / LOAD_PCT]) */
	if (check_mul_overflow(limit, 100, &count)				||
	    ((count /= LOAD_PCT), 0)						||
	    check_mul_overflow(count, sizeof(struct hash_table_entry), &bytes)	||
	    check_add_overflow(bytes, sizeof(struct hash_table), &bytes))
		return NULL;

	ht = malloc(bytes);
	if (ht) {
		get_random(&ht->seed, sizeof(ht->seed));
		ht->count = count;
		htable_clear(ht);
	}

	return ht;
}

static unsigned long hashed_key_entry(struct hash_table *ht, u64 key)
{
	return xxh64(&key, sizeof(key), ht->seed) % ht->count;
}

/*
 * Iterate over all the entries in the hash table, starting from the
 * given entry index.  This weird construction lets us avoid division to
 * wrap the index on every loop iteration.  We're performing iteration
 * through two intervals in the array: first [start,count), then
 * [0,start) if start wasn't 0.
 *
 * ht and start are only evaluated once.  i, end, and tmp are evaluated
 * and assigned to as lvalues multiple times.
 */
#define for_each_entry(ht, start, i, end, tmp)						\
	for (i = (start), tmp = i, end = (ht)->count;					\
	     (i < end) || ((tmp > 0) && ({ i = 0; end = tmp; tmp = 0; true; }));	\
	     i++)


/*
 * Insert the given key and value into the hash table.  If the key is
 * already present then the value is updated.
 *
 * The caller cannot store keys with the high bits set that we use for
 * the distance.  This lets us use mostly aligned u64s in the hash table
 * entries.  The caller also can't use the value 0, this lets us use a
 * non-zero value as the indication that an entry is populated (and is
 * what we return in _foreach.. a peculiar interface that's been usable
 * so far.)
 *
 * The caller is responsible for honoring the capacity limit.
 */
void htable_insert(struct hash_table *ht, u64 key, u64 val)
{
	struct hash_table_entry ins = {
		.dist = 0,
		.key = key,
		.val = val,
	};
	unsigned long end;
	unsigned long tmp;
	unsigned long i;

	BUG_ON(key > KEY_MAX || val == 0);

	for_each_entry(ht, hashed_key_entry(ht, key), i, end, tmp) {
		if (ht->entries[i].val == 0 || ht->entries[i].key == key) {
			ht->entries[i] = ins;
			break;
		}

		if (ins.dist > ht->entries[i].dist)
			swap(ins, ht->entries[i]);

		ins.dist++;
	}
}

u64 htable_lookup(struct hash_table *ht, u64 key)
{
	unsigned long dist;
	unsigned long end;
	unsigned long tmp;
	unsigned long i;

	dist = 0;
	for_each_entry(ht, hashed_key_entry(ht, key), i, end, tmp) {
		if (ht->entries[i].key == key)
			return ht->entries[i].val;

		if (ht->entries[i].val == 0 || dist > ht->entries[i].dist)
			break;

		dist++;
	}

	return 0;
}

void htable_foreach_init(struct hash_table *ht, unsigned long *fe)
{
	*fe = 0;
}

/*
 * After _foreach_init, return all the values stored in the table
 * through successive calls.  The values are returned in the order of
 * their hashed key values, so effectively random.  Returns 0 when there
 * are no more values.
 */
u64 htable_foreach(struct hash_table *ht, unsigned long *fe)
{
	u64 val = 0;

	while (!val && *fe < ht->count)
		val = ht->entries[(*fe)++].val;

	return val;
}

/*
 * Removes the key and it's value from the hash table, if found.  Before
 * zeroing a deleted entry we first check if we can swap it with the
 * next entry which decreases the next entry's non-zero distance.
 */
void htable_delete(struct hash_table *ht, u64 key)
{
	unsigned long dist;
	unsigned long del;
	unsigned long end;
	unsigned long tmp;
	unsigned long i;

	dist = 0;
	for_each_entry(ht, hashed_key_entry(ht, key), i, end, tmp) {
		if (ht->entries[i].key == key) {
			del = i;
			for_each_entry(ht, del + 1, i, end, tmp) {
				if (ht->entries[i].dist == 0)
					break;

				ht->entries[del] = ht->entries[i];
				ht->entries[del].dist--;
				del = i;
			}

			ht->entries[del] = (struct hash_table_entry) { 0, };
			return;
		}

		if (ht->entries[i].val == 0 || dist > ht->entries[i].dist)
			return;

		dist++;
	}
}

/*
 * This is obviously expensive so it's only used for reasonably small
 * tables.
 */
void htable_clear(struct hash_table *ht)
{
	memset(ht->entries, 0, ht->count * sizeof(ht->entries[0]));
}
