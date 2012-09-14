/*
 * Copyright (C) 2012 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#include "dm-cache-policy.h"
#include "dm.h"

#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>

//#define debug(x...) pr_alert(x)
#define debug(x...) ;

#define DM_MSG_PREFIX "cache-policy-arc"

/*----------------------------------------------------------------*/

static unsigned next_power(unsigned n, unsigned min)
{
	unsigned r = min;

	while (r < n)
		r <<= 1;

	return r;
}

/*----------------------------------------------------------------*/

static unsigned long *alloc_bitset(unsigned nr_entries, bool set_to_ones)
{
	size_t s = sizeof(unsigned long) * dm_div_up(nr_entries, BITS_PER_LONG);
	unsigned long *r = vzalloc(s);
	if (r && set_to_ones)
		memset(r, ~0, s);

	return r;
}

static void free_bitset(unsigned long *bits)
{
	vfree(bits);
}

/*----------------------------------------------------------------*/

/*
 * Multiqueue
 * FIXME: explain
 */
#define NR_MQ_LEVELS 16

typedef unsigned (*queue_level_fn)(void *context, struct list_head *entry, unsigned nr_levels);

struct multiqueue {
	queue_level_fn queue_level;
	void *context;

	struct list_head qs[NR_MQ_LEVELS];
};

static void mq_init(struct multiqueue *mq,
		    queue_level_fn queue_level,
		    void *context)
{
	unsigned i;

	mq->queue_level = queue_level;
	mq->context = context;

	for (i = 0; i < NR_MQ_LEVELS; i++)
		INIT_LIST_HEAD(mq->qs + i);
}

static struct list_head *mq_get_q(struct multiqueue *mq, struct list_head *elt)
{
	unsigned level = mq->queue_level(mq->context, elt, NR_MQ_LEVELS);
	BUG_ON(level >= NR_MQ_LEVELS);
	return mq->qs + level;
}

static void mq_push(struct multiqueue *mq, struct list_head *elt)
{
	list_add_tail(elt, mq_get_q(mq, elt));
}

static void mq_remove(struct list_head *elt)
{
	list_del(elt);
}

/*
 * Gives us the oldest entry of the lowest level.
 */
static struct list_head *mq_pop(struct multiqueue *mq)
{
	unsigned i;
	struct list_head *r;

	for (i = 0; i < NR_MQ_LEVELS; i++)
		if (!list_empty(mq->qs + i)) {
			r = mq->qs[i].next;
			list_del(r);
			return r;
		}

	return NULL;
}

static void mq_demote(struct multiqueue *mq)
{
	unsigned level;

	for (level = 1; level < NR_MQ_LEVELS; level++)
		list_splice_init(mq->qs + level, mq->qs + level - 1);
}

/*----------------------------------------------------------------*/

struct entry {
	struct hlist_node hlist;
	struct list_head list;
	dm_block_t oblock;
	dm_block_t cblock;

	bool in_cache:1;
	unsigned hit_count;
	unsigned tick;
};

struct seen_block {
	dm_block_t oblock;
	unsigned tick;
};

struct arc_policy {
	struct dm_cache_policy policy;

	dm_block_t cache_size;
	unsigned tick;
	unsigned hits;

	spinlock_t lock;

	struct multiqueue mq_pre_cache;
	struct multiqueue mq_cache;
	unsigned demote_period_mask;

	/*
	 * We know exactly how many entries will be needed, so we can
	 * allocate them up front.
	 */
	unsigned nr_entries;
	unsigned nr_allocated;
	struct entry *entries;

	unsigned long *allocation_bitset;
	unsigned nr_cblocks_allocated;

	unsigned nr_buckets;
	dm_block_t hash_mask;
	struct hlist_head *table;

	/* Fields for tracking IO pattern */
	/* 0: IO stream is random. 1: IO stream is sequential */
	bool seq_stream;
	unsigned nr_seq_samples, nr_rand_samples;
	dm_block_t last_end_oblock;
	unsigned int seq_io_threshold;

	/* Last looked up cached entry */
	struct entry *last_lookup;
};

#define NR_PRE_CACHE_LEVELS 4

static struct arc_policy *to_arc_policy(struct dm_cache_policy *p)
{
	return container_of(p, struct arc_policy, policy);
}

static void arc_destroy(struct dm_cache_policy *p)
{
	struct arc_policy *a = to_arc_policy(p);

	free_bitset(a->allocation_bitset);
	kfree(a->table);
	vfree(a->entries);
	kfree(a);
}

/*----------------------------------------------------------------*/

/* FIXME: replace with the new hash table stuff */

static unsigned hash(struct arc_policy *a, dm_block_t b)
{
	const dm_block_t BIG_PRIME = 4294967291UL;
	dm_block_t h = b * BIG_PRIME;

	return (uint32_t) (h & a->hash_mask);
}

static void __hash_insert(struct arc_policy *a, struct entry *e)
{
	unsigned h = hash(a, e->oblock);
	hlist_add_head(&e->hlist, a->table + h);
}

static struct entry *__hash_lookup(struct arc_policy *a, dm_block_t origin)
{
	unsigned h = hash(a, origin);
	struct hlist_head *bucket = a->table + h;
	struct hlist_node *tmp;
	struct entry *e;

	/* Check last lookup cache */
	if (a->last_lookup && a->last_lookup->oblock == origin)
		return a->last_lookup;

	hlist_for_each_entry(e, tmp, bucket, hlist)
		if (e->oblock == origin) {
			a->last_lookup = e;
			return e;
		}

	return NULL;
}

static void __hash_remove(struct arc_policy *a, struct entry *e)
{
	hlist_del(&e->hlist);
}

/*----------------------------------------------------------------*/

static struct entry *__arc_alloc_entry(struct arc_policy *a)
{
	struct entry *e;

	if (a->nr_allocated >= a->nr_entries)
		return NULL;

	e = a->entries + a->nr_allocated;
	INIT_LIST_HEAD(&e->list);
	INIT_HLIST_NODE(&e->hlist);
	a->nr_allocated++;
	e->tick = a->tick;

	return e;
}

static void __alloc_cblock(struct arc_policy *a, dm_block_t cblock)
{
	BUG_ON(cblock > a->cache_size);
	BUG_ON(test_bit(cblock, a->allocation_bitset));
	set_bit(cblock, a->allocation_bitset);
	a->nr_cblocks_allocated++;
}

static void __free_cblock(struct arc_policy *a, dm_block_t cblock)
{
	BUG_ON(cblock > a->cache_size);
	BUG_ON(!test_bit(cblock, a->allocation_bitset));
	clear_bit(cblock, a->allocation_bitset);
	a->nr_cblocks_allocated--;
}

/*
 * This doesn't allocate the block.
 */
static int __find_free_cblock(struct arc_policy *a, dm_block_t *result)
{
	int r = -ENOSPC;
	unsigned nr_words = dm_div_up(a->cache_size, BITS_PER_LONG);
	unsigned w, b;

	if (a->nr_cblocks_allocated >= a->cache_size)
		return -ENOSPC;

	for (w = 0; w < nr_words; w++) {
		/*
		 * ffz is undefined if no zero exists
		 */
		if (a->allocation_bitset[w] != ~0UL) {
			b = ffz(a->allocation_bitset[w]);

			*result = (w * BITS_PER_LONG) + b;
			if (*result < a->cache_size)
				r = 0;

			break;
		}
	}

	return r;
}

static bool __any_free_cblocks(struct arc_policy *a)
{
	return a->nr_cblocks_allocated < a->cache_size;
}

/*----------------------------------------------------------------*/

static void __arc_push(struct arc_policy *a, struct entry *e)
{
	e->tick = a->tick;
	__hash_insert(a, e);

	if (e->in_cache) {
		__alloc_cblock(a, e->cblock);
		mq_push(&a->mq_cache, &e->list);
	} else
		mq_push(&a->mq_pre_cache, &e->list);
}


static void __arc_del(struct arc_policy *a, struct entry *e)
{
	mq_remove(&e->list);
	__hash_remove(a, e);
	if (e->in_cache)
		__free_cblock(a, e->cblock);
}

// FIXME: move up with the structs
enum queue_area {
	QA_PRE_CACHE,
	QA_CACHE
};

static struct entry *__arc_pop(struct arc_policy *a, enum queue_area area)
{
	struct entry *e;

	if (area == QA_PRE_CACHE)
		e = container_of(mq_pop(&a->mq_pre_cache), struct entry, list);
	else
		e = container_of(mq_pop(&a->mq_cache), struct entry, list);

	if (e) {
		__hash_remove(a, e);

		if (e->in_cache)
			__free_cblock(a, e->cblock);
	}

	return e;
}

static bool arc_random_stream(struct arc_policy *a)
{
	return !a->seq_stream;
}

static void __arc_update_io_stream_data(struct arc_policy *a, struct bio *bio)
{
	if (bio->bi_sector == a->last_end_oblock + 1) {
		/* Block sequential to last io */
		a->nr_seq_samples++;
	} else {
		/* One non sequential IO resets the existing data */
		if (a->nr_seq_samples) {
			a->nr_seq_samples = 0;
			a->nr_rand_samples = 0;
		}
		a->nr_rand_samples++;
	}

	a->last_end_oblock = bio->bi_sector + bio_sectors(bio) - 1;

	/*
	 * If current stream state is sequential and we see 4 random IO,
	 * change state. Otherwise if current state is random and we see
	 * seq_io_threshold sequential IO, change stream state to sequential.
	 */

	if (a->seq_stream && a->nr_rand_samples >= 4) {
		a->seq_stream = false;
		debug("switched stream state to random. nr_rand=%u"
			" nr_seq=%u\n", a->nr_rand_samples, a->nr_seq_samples);
		a->nr_seq_samples = a->nr_rand_samples = 0;
	} else if (!a->seq_stream && a->seq_io_threshold &&
                   a->nr_seq_samples >= a->seq_io_threshold) {
		a->seq_stream = true;
		debug("switched stream state to sequential. nr_rand=%u"
			" nr_seq=%u\n", a->nr_rand_samples, a->nr_seq_samples);
		a->nr_seq_samples = a->nr_rand_samples = 0;
	}
}

static bool updated_this_tick(struct arc_policy *a, struct entry *e)
{
	return a->tick == e->tick;
}

static void __arc_hit(struct arc_policy *a, struct entry *e)
{
	if (updated_this_tick(a, e))
		return;

	__arc_del(a, e);
	e->hit_count++;
	__arc_push(a, e);

	if (!(++a->hits & a->demote_period_mask)) {
		pr_alert("running mq_demote\n");
		mq_demote(&a->mq_cache);
		mq_demote(&a->mq_pre_cache);
		a->hits = 0;
	}
}

static dm_block_t demote_cblock(struct arc_policy *a, dm_block_t *oblock)
{
	dm_block_t result;
	struct entry *demoted = __arc_pop(a, QA_CACHE);

	BUG_ON(!demoted);
	result = demoted->cblock;
	*oblock = demoted->oblock;
	demoted->in_cache = false;
	__arc_push(a, demoted);

	return result;
}

#define PROMOTE_THRESHOLD 128

static bool should_promote(struct arc_policy *a,
			   struct entry *e,
			   bool can_migrate,
			   bool cheap_copy)
{
	bool possible_migration = can_migrate && (e->hit_count >= PROMOTE_THRESHOLD);
	bool possible_new = cheap_copy && __any_free_cblocks(a);
	bool promote = arc_random_stream(a) && (possible_new || possible_migration);

	return promote;
}

// FIXME: rename origin_block to oblock
static void __arc_map_found(struct arc_policy *a,
			    struct entry *e,
			    dm_block_t origin_block,
			    bool can_migrate,
			    bool cheap_copy,
			    struct policy_result *result)
{
	dm_block_t cblock;
	bool updated = updated_this_tick(a, e); /* has to be done before __arc_hit */

	__arc_hit(a, e);

	if (e->in_cache) {
		result->op = POLICY_HIT;
		result->cblock = e->cblock;
		return;
	}

	if (updated || !should_promote(a, e, can_migrate, cheap_copy)) {
		result->op = POLICY_MISS;
		return;
	}

	if (__find_free_cblock(a, &cblock) == -ENOSPC) {
		result->op = POLICY_REPLACE;
		cblock = demote_cblock(a, &result->old_oblock);
	} else {
		result->op = POLICY_NEW;
	}

	result->cblock = e->cblock = cblock;

	__arc_del(a, e);
	e->in_cache = true;
	__arc_push(a, e);
}

static void to_pre_cache(struct arc_policy *a,
			 dm_block_t oblock,
			 struct policy_result *result)
{
	struct entry *e = __arc_alloc_entry(a);

	if (!e)
		e = __arc_pop(a, QA_PRE_CACHE);

	if (unlikely(!e)) {
		DMWARN("couldn't pop from pre cache");
		return;
	}

	e->in_cache = false;
	e->oblock = oblock;
	e->hit_count = 1;
	__arc_push(a, e);

	result->op = POLICY_MISS;
}

static void straight_to_cache(struct arc_policy *a,
			      dm_block_t oblock,
			      struct policy_result *result)
{
	struct entry *e = __arc_alloc_entry(a);

	if (unlikely(!e))
		return;

	e->oblock = oblock;
	e->hit_count = 1;

	if (__find_free_cblock(a, &e->cblock) == -ENOSPC) {
		DMWARN("straight_to_cache couldn't allocate cblock");
		result->op = POLICY_MISS;
		e->in_cache = false;
	} else {
		result->op = POLICY_NEW;
		result->cblock = e->cblock;
		e->in_cache = true;
	}

	__arc_push(a, e);
}

static void __arc_map(struct arc_policy *a,
		      dm_block_t oblock,
		      int data_dir,
		      bool can_migrate,
		      bool cheap_copy,
		      struct policy_result *result)
{
	struct entry *e = __hash_lookup(a, oblock);
	if (e) {
		__arc_map_found(a, e, oblock, can_migrate, cheap_copy, result);
		return;
	}

	// FIXME: wrong place, and I'm not sure about the sequential stuff anyway
	if (!arc_random_stream(a)) {
		result->op = POLICY_MISS;
		return;
	}

	if (cheap_copy && __any_free_cblocks(a))
		straight_to_cache(a, oblock, result);
	else
		to_pre_cache(a, oblock, result);
}

static void arc_map(struct dm_cache_policy *p, dm_block_t origin_block, int data_dir,
		    bool can_migrate, bool cheap_copy, struct bio *bio,
		    struct policy_result *result)
{
	unsigned long flags;
	struct arc_policy *a = to_arc_policy(p);

	spin_lock_irqsave(&a->lock, flags);
	__arc_update_io_stream_data(a, bio);
	__arc_map(a, origin_block, data_dir, can_migrate, cheap_copy, result);
	spin_unlock_irqrestore(&a->lock, flags);
}

static int arc_load_mapping(struct dm_cache_policy *p, dm_block_t oblock, dm_block_t cblock)
{
	struct arc_policy *a = to_arc_policy(p);
	struct entry *e;

	debug("loading mapping %lu -> %lu\n",
	      (unsigned long) oblock,
	      (unsigned long) cblock);

	e = __arc_alloc_entry(a);
	if (!e)
		return -ENOMEM;

	e->cblock = cblock;
	e->oblock = oblock;
	e->in_cache = true;
	__arc_push(a, e);

	return 0;
}

static void arc_remove_mapping(struct dm_cache_policy *p, dm_block_t oblock)
{
	struct arc_policy *a = to_arc_policy(p);
	struct entry *e = __hash_lookup(a, oblock);

	BUG_ON(!e || e->in_cache);

	__arc_del(a, e);
	e->in_cache = false;
	__arc_push(a, e);
}

static void arc_force_mapping(struct dm_cache_policy *p,
		dm_block_t current_oblock, dm_block_t new_oblock)
{
	struct arc_policy *a = to_arc_policy(p);
	struct entry *e = __hash_lookup(a, current_oblock);

	BUG_ON(!e || e->in_cache);

	__arc_del(a, e);
	e->oblock = new_oblock;
	__arc_push(a, e);
}

static dm_block_t arc_residency(struct dm_cache_policy *p)
{
	struct arc_policy *a = to_arc_policy(p);
	return a->nr_cblocks_allocated;
}

static void arc_set_seq_io_threshold(struct dm_cache_policy *p,
			unsigned int seq_io_thresh)
{
	struct arc_policy *a = to_arc_policy(p);

	a->seq_io_threshold = seq_io_thresh;
}

static void arc_tick(struct dm_cache_policy *p)
{
	struct arc_policy *a = to_arc_policy(p);
	unsigned long flags;

	spin_lock_irqsave(&a->lock, flags);
	a->tick++;
	spin_unlock_irqrestore(&a->lock, flags);
}

static unsigned arc_queue_level(void *context, struct list_head *elt, unsigned nr_levels)
{
	struct entry *e = container_of(elt, struct entry, list);
	return min((unsigned) ilog2(e->hit_count), nr_levels);
}

static struct dm_cache_policy *arc_create(dm_block_t cache_size)
{
	struct arc_policy *a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return NULL;

	a->policy.destroy = arc_destroy;
	a->policy.map = arc_map;
	a->policy.load_mapping = arc_load_mapping;
	a->policy.remove_mapping = arc_remove_mapping;
	a->policy.force_mapping = arc_force_mapping;
	a->policy.residency = arc_residency;
	a->policy.set_seq_io_threshold = arc_set_seq_io_threshold;
	a->policy.tick = arc_tick;

	a->cache_size = cache_size;
	a->tick = 0;
	a->hits = 0;
	spin_lock_init(&a->lock);

	mq_init(&a->mq_pre_cache, arc_queue_level, a);
	mq_init(&a->mq_cache, arc_queue_level, a);
	a->demote_period_mask = next_power(cache_size, 1024) - 1;

	a->last_lookup = NULL;

	a->nr_entries = 3 * cache_size;
	a->entries = vzalloc(sizeof(*a->entries) * a->nr_entries);
	if (!a->entries) {
		kfree(a);
		return NULL;
	}

	a->nr_allocated = 0;
	a->nr_cblocks_allocated = 0;

	a->nr_buckets = next_power(cache_size / 4, 16);
	a->hash_mask = a->nr_buckets - 1;
	a->table = kzalloc(sizeof(*a->table) * a->nr_buckets, GFP_KERNEL);
	if (!a->table) {
		vfree(a->entries);
		kfree(a);
		return NULL;
	}

	a->allocation_bitset = alloc_bitset(cache_size, 0);
	if (!a->allocation_bitset) {
		kfree(a->table);
		vfree(a->entries);
		kfree(a);
		return NULL;
	}

	return &a->policy;
}

/*----------------------------------------------------------------*/

// FIXME: register this under the 'default' policy name too

static struct dm_cache_policy_type arc_policy_type = {
	.name = "arc",
	.owner = THIS_MODULE,
        .create = arc_create
};

static int __init arc_init(void)
{
	return dm_cache_policy_register(&arc_policy_type);
}

static void __exit arc_exit(void)
{
	dm_cache_policy_unregister(&arc_policy_type);
}

module_init(arc_init);
module_exit(arc_exit);

MODULE_AUTHOR("Joe Thornber");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("arc+ cache policy");

/*----------------------------------------------------------------*/
