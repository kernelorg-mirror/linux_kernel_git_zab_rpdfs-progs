/* SPDX-License-Identifier: GPL-2.0 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "shared/lk/build_bug.h"
#include "shared/lk/bug.h"
#include "shared/lk/limits.h"
#include "shared/lk/minmax.h"
#include "shared/lk/overflow.h"
#include "shared/lk/types.h"
#include "shared/lk/xxhash.h"
#include "shared/dtracef.h"
#include "shared/get_random.h"
#include "shared/hash_table.h"

/*
 * A resizing hash table that uses robin hood hashing.  Each table
 * stores pointers to one type of object which contains a large key.  We
 * store the hash of the key in the table so that we we're unlikely to
 * perform deep key comparison while probing.
 *
 * The table grows and shrinks as it crosses thresholds.  It migrates a
 * fraction of entries from the old table to the new in each call,
 * amortizing the cost.
 */

/*
 * Seeds are consistent across resized versions of a caller's table so
 * that we don't have to recalculate the key's hash as we migrate
 * entries between tables.
 */
struct hash_table {
	struct hash_table *prev_ht;
	unsigned long seed;
	unsigned long count;
	unsigned long size;
	unsigned long low_util_count;
	unsigned long high_util_count;
	unsigned long migrate_pos;
	size_t key_offset;
	size_t key_size;
	struct hash_table_entry {
		unsigned long probe_len:8,
			      hash;
		void *obj;
	} *entries;
};

/* make the min size around a reasonable chunk of l1 */
#define MIN_SIZE	((32 * 1024) / sizeof(struct hash_table_entry))

/*
 * We're trying to keep the utilization in a reasonable band, to avoid
 * spending all our time resizing, and to leave equal distance between
 * resize thresholds so that we have time to resize and to avoid
 * flapping.
 *
 * The migration batch then has to be a large enough factor such that if
 * all the caller does is keep modifying then a resize will be done
 * before the next one hits.  But it can't be too large because it
 * scatters the entries all over the destination table and we want to
 * amortize that cost across calls.
 */
#define PCT(a, b)	(((a) * (b)) / 100)
#define LOW_UTIL	45
#define HIGH_UTIL	90
#define GROW		150
#define SHRINK		66
#define MIGRATE_BATCH	16  /* count, not a pct :) */

static bool obj_keys_eq(struct hash_table *ht, struct hash_table_entry *a,
			struct hash_table_entry *b)
{
	return a->hash == b->hash &&
	       memcmp(a->obj + ht->key_offset, b->obj + ht->key_offset, ht->key_size) == 0;
}

/*
 * Calculate a fake object pointer so that we can use one key equality
 * function that works on entries with hashes and object pointers.
 */
static void *fake_obj(struct hash_table *ht, void *key)
{
	return key - ht->key_offset;
}

static unsigned long calc_key_hash(struct hash_table *ht, void *key)
{
#if BITS_PER_LONG == 64
	return xxh64(key, ht->key_size, ht->seed);
#else
	return xxh32(key, ht->key_size, ht->seed);
#endif
}

static unsigned long calc_obj_hash(struct hash_table *ht, void *obj)
{
	return calc_key_hash(ht, obj + ht->key_offset);
}

static struct hash_table *alloc_table(size_t size, unsigned long seed,
				      size_t key_offset, size_t key_size)
{
	struct hash_table *ht;

	ht = calloc(1, sizeof(struct hash_table));
	if (ht) {
		ht->entries = calloc(size, sizeof(struct hash_table_entry));
		if (!ht->entries) {
			free(ht);
			ht = NULL;
		} else {
			ht->seed = seed;
			ht->size = size;
			ht->low_util_count = PCT(size, LOW_UTIL);
			if (ht->low_util_count <= MIN_SIZE)
				ht->low_util_count = 0;
			ht->high_util_count = PCT(size, HIGH_UTIL);
			ht->key_offset = key_offset;
			ht->key_size = key_size;
		}
	}

	return ht;
}

static void free_table(struct hash_table *ht)
{
	if (ht) {
		free_table(ht->prev_ht);
		free(ht->entries);
		free(ht);
	}
}

struct hash_table *htable_alloc(size_t key_offset, size_t key_size)
{
	unsigned long seed;

	get_random(&seed, sizeof(seed));
	return alloc_table(MIN_SIZE, seed, key_offset, key_size);
}

/*
 * Iterate over entries, wrapping around the end, indefinitely.  Starts
 * at the start index.  The probe length is initialized to 0 and
 * incremented after each iteration.
 *
 * ht, ent, and pl are evaluated a lot.. keep them simple variables.
 */
#define for_each_entry(ht, start, ent, pl) \
	for (pl = 0, ent = &ht->entries[(start) % ht->size]; ; \
	     pl++, ent = (++ent == &ht->entries[ht->size] ? ht->entries : ent))

/* we rely on empty entries having 0 probe_len, wipe the whole entry */
static inline void clear_entry(struct hash_table_entry *ent)
{
	*ent = (struct hash_table_entry) { 0, };
}

/*
 * Insert an entry into the table.  Silently overwrites an existing
 * object that matches the insertion key.
 */
static void insert_entry(struct hash_table *ht, unsigned long hash, void *obj)
{
	struct hash_table_entry *ent;
	struct hash_table_entry ins = {
		.hash = hash,
		.obj = obj,
	};

	for_each_entry(ht, ins.hash, ent, ins.probe_len) {
		if (!ent->obj) {
			*ent = ins;
			ht->count++;
			return;
		}

		if (obj_keys_eq(ht, ent, &ins)) {
			*ent = ins;
			return;
		}

		if (ins.probe_len > ent->probe_len)
			swap(ins, *ent);
	}
}

/*
 * Only overwrite an existing entry with the insertion key.  Returns
 * true if it found a match and updated the object.  Returns false if
 * the key wasn't present.
 */
static bool overwrite_entry(struct hash_table *ht, unsigned long hash, void *obj)
{
	struct hash_table_entry *ent;
	struct hash_table_entry ins = {
		.hash = hash,
		.obj = obj,
	};

	for_each_entry(ht, ins.hash, ent, ins.probe_len) {
		if (!ent->obj || ins.probe_len > ent->probe_len)
			return false;

		if (obj_keys_eq(ht, ent, &ins)) {
			*ent = ins;
			return true;
		}
	}
}

static void *lookup_entry(struct hash_table *ht, unsigned long hash, void *key)
{
	struct hash_table_entry *ent;
	struct hash_table_entry look = {
		.hash = hash,
		.obj = fake_obj(ht, key),
	};

	for_each_entry(ht, look.hash, ent, look.probe_len) {
		if (!ent->obj || look.probe_len > ent->probe_len)
			return NULL;

		if (obj_keys_eq(ht, ent, &look))
			return ent->obj;
	}
}

/*
 * Removes the key and it's value from the hash table, if found.  Before
 * zeroing a deleted entry we try to swap it with successive entries to
 * decreases their non-zero probe length.
 */
static bool delete_entry(struct hash_table *ht, unsigned long hash, void *key)
{
	struct hash_table_entry *clear;
	struct hash_table_entry *ent;
	struct hash_table_entry del = {
		.hash = hash,
		.obj = fake_obj(ht, key),
	};

	for_each_entry(ht, del.hash, ent, del.probe_len) {
		if (!ent->obj || del.probe_len > ent->probe_len)
			return false;

		if (obj_keys_eq(ht, ent, &del)) {
			clear = ent;
			break;
		}
	}

	for_each_entry(ht, (clear - ht->entries) + 1, ent, del.probe_len) {
		if (!ent->obj || ent->probe_len == 0)
			break;

		*clear = *ent;
		clear->probe_len--;
		clear = ent;
	}

	clear_entry(clear);
	ht->count--;
	return true;
}

/*
 * Migrate a batch of entries from the src table to the dst.
 *
 * We always examine at least a full batch of entries.  This ensures
 * that we'll finish migrating before a resized table hits its next
 * resize threshold.
 *
 * Then we'll only stop moving a contiguous run of entries when they're
 * empty or have a probe length of 0.  This lets us clear all the
 * entries we moved without having to perform the full deletion process
 * that could have to decrease later probe lengths.
 *
 * To that end, we have to be careful not to start the move in the
 * middle of a run of entries.
 */
static void migrate_batch(struct hash_table *dst, struct hash_table *src)
{
	struct hash_table_entry *ent;
	unsigned long pos;
	unsigned long pl;

	/* skip back to the start of a run */
	for_each_entry(src, src->migrate_pos, ent, pl) {
		pos = src->migrate_pos - ent->probe_len;
		break;
	}

	/* migrate until we hit entries that wouldn't need to be moved back */
	for_each_entry(src, pos, ent, pl) {
		if (pl >= MIGRATE_BATCH && ent->probe_len == 0)
			break;

		if (ent->obj) {
			insert_entry(dst, ent->hash, ent->obj);
			clear_entry(ent);
			src->count--;
		}
	}

	src->migrate_pos = pos + pl;
}

static void manage_resize(struct hash_table *ht)
{
	struct hash_table *re;
	unsigned long size;

	if (ht->prev_ht) {
		migrate_batch(ht, ht->prev_ht);
		if (ht->prev_ht->count == 0) {
			dtracef("hash_table_resize_stop", "prev_size %lu size %lu",
				ht->prev_ht->size, ht->size);
			free_table(ht->prev_ht);
			ht->prev_ht = NULL;
		}
	}

	if (!ht->prev_ht) {
		if (ht->count > ht->high_util_count)
			size = PCT(ht->size, GROW);
		else if (ht->count < ht->low_util_count)
			size = max(PCT(ht->size, SHRINK), MIN_SIZE);
		else
			return;

		re = alloc_table(size, ht->seed, ht->key_offset, ht->key_size);
		BUG_ON(!re);
		swap(*ht, *re);
		ht->prev_ht = re;

		dtracef("hash_table_resize_start", "prev_size %lu size %lu",
			ht->prev_ht->size, ht->size);
	}
}

void htable_insert(struct hash_table *ht, void *obj)
{
	unsigned long hash = calc_obj_hash(ht, obj);

	if (!ht->prev_ht || !overwrite_entry(ht->prev_ht, hash, obj))
		insert_entry(ht, hash, obj);

	manage_resize(ht);
}

void *htable_lookup(struct hash_table *ht, void *key)
{
	unsigned long hash = calc_key_hash(ht, key);
	void *obj;

	if (!ht->prev_ht || (obj = lookup_entry(ht->prev_ht, hash, key)) == NULL)
		obj = lookup_entry(ht, hash, key);

	manage_resize(ht);
	return obj;
}

void htable_delete(struct hash_table *ht, void *key)
{
	unsigned long hash = calc_key_hash(ht, key);

	if (!ht->prev_ht || !delete_entry(ht->prev_ht, hash, key))
		delete_entry(ht, hash, key);

	manage_resize(ht);
}

/*
 * Free a previously allocated table.  It's safe to call this on a null
 * table pointer.  The caller can have a callback called for every
 * object that was present in the table as it was destroyed.
 *
 * The hash table is unusable once this is called.  The callback
 * shouldn't call htable functions on this table.
 */
void htable_destroy(struct hash_table *ht, htable_callback_t cb, void *arg)
{
	struct hash_table_entry *ent;
	unsigned long pl;

	if (ht) {
		htable_destroy(ht->prev_ht, cb, arg);

		if (cb) {
			for_each_entry(ht, 0, ent, pl) {
				if (ent->obj) {
					cb(ent->obj, arg);
					ht->count--;
				}
				if (ht->count == 0)
					break;
			}
		}

		free_table(ht);
	}
}

__unused
static int htable_test(void)
{
	struct hash_table *ht;
	struct tobj {
		u32 x;
	} *objs, *o;
	u32 nr = 10000;
	s32 i;

	ht = htable_alloc(offsetof(struct tobj, x), sizeof_field(struct tobj, x));
	objs = malloc(nr * sizeof(struct tobj));
	if (!ht || !objs) {
		printf("couldn't alloc\n");
		exit(1);
	}

	for (i = 0; i < nr; i++) {
		objs[i].x = i;

		o = htable_lookup(ht, &i);
		if (o != NULL) {
			printf("pre insert present i %u obj %zu\n", i, o - objs);
			exit(1);
		}

		htable_insert(ht, &objs[i]);

		o = htable_lookup(ht, &i);
		if (o == NULL) {
			printf("post insert null i %u\n", i);
			exit(1);
		}
		if (o != &objs[i] || o->x != i) {
			printf("post insert bad obj i %u obj %zu x %u\n", i, o - objs, o->x);
			exit(1);
		}
	}

	for (i = nr - 1; i >= 0; i--) {
		o = htable_lookup(ht, &i);
		if (o == NULL) {
			printf("pre delete null i %u\n", i);
			exit(1);
		}
		if (o != &objs[i] || o->x != i) {
			printf("pre delete bad obj i %u obj %zu x %u\n", i, o - objs, o->x);
			exit(1);
		}

		htable_delete(ht, &objs[i].x);

		o = htable_lookup(ht, &i);
		if (o != NULL) {
			printf("post delete present i %u obj %zu\n", i, o - objs);
			exit(1);
		}
	}

	htable_destroy(ht, NULL, NULL);
	free(objs);

	printf("OK\n");
	exit(0);
}

