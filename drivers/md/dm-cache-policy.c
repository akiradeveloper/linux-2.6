/*
 * Copyright (C) 2012 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#include "dm-cache-policy.h"
#include "dm.h"

#include <linux/list.h>
#include <linux/slab.h>

//#define debug(x...) pr_alert(x)
#define debug(x...) ;

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

struct queue {
	unsigned size;
	struct list_head elts;
};

static void queue_init(struct queue *q)
{
	q->size = 0;
	INIT_LIST_HEAD(&q->elts);
}

static unsigned queue_size(struct queue *q)
{
	return q->size;
}

static bool queue_empty(struct queue *q)
{
	BUG_ON(q->size ? list_empty(&q->elts) : !list_empty(&q->elts));
	return !q->size;
}

static struct list_head *queue_pop(struct queue *q)
{
	struct list_head *r;

	BUG_ON(list_empty(&q->elts));
	r = q->elts.next;
	list_del(r);
	q->size--;

	return r;
}

static void queue_del(struct queue *q, struct list_head *elt)
{
	BUG_ON(!q->size);
	list_del(elt);
	q->size--;
}

static void queue_push(struct queue *q, struct list_head *elt)
{
	list_add_tail(elt, &q->elts);
	q->size++;
}

/*----------------------------------------------------------------*/

enum arc_state {
	ARC_B1,
	ARC_T1,
	ARC_B2,
	ARC_T2
};

struct arc_entry {
	enum arc_state state;
	struct hlist_node hlist;
	struct list_head list;
	dm_block_t oblock;
	dm_block_t cblock;
};

struct arc_policy {
	struct dm_cache_policy policy;

	dm_block_t cache_size;

	spinlock_t lock;

	dm_block_t p;		/* the magic factor that balances lru vs lfu */
	struct queue b1, t1, b2, t2;

	/*
	 * We know exactly how many entries will be needed, so we can
	 * allocate them up front.
	 */
	struct arc_entry *entries;
	unsigned long *allocation_bitset;
	dm_block_t nr_allocated;

	unsigned nr_buckets;
	dm_block_t hash_mask;
	struct hlist_head *table;

	dm_block_t interesting_size;
	dm_block_t *interesting_blocks;
	dm_block_t last_lookup;
};

static struct arc_policy *to_arc_policy(struct dm_cache_policy *p)
{
	return container_of(p, struct arc_policy, policy);
}

static void arc_destroy(struct dm_cache_policy *p)
{
	struct arc_policy *a = to_arc_policy(p);

	free_bitset(a->allocation_bitset);
	vfree(a->interesting_blocks);
	kfree(a->table);
	vfree(a->entries);
	kfree(a);
}

static unsigned hash(struct arc_policy *a, dm_block_t b)
{
	const dm_block_t BIG_PRIME = 4294967291UL;
	dm_block_t h = b * BIG_PRIME;

	return (uint32_t) (h & a->hash_mask);
}

static void __arc_insert(struct arc_policy *a, struct arc_entry *e)
{
	unsigned h = hash(a, e->oblock);
	hlist_add_head(&e->hlist, a->table + h);
}

static struct arc_entry *__arc_lookup(struct arc_policy *a, dm_block_t origin)
{
	unsigned h = hash(a, origin);
	struct hlist_head *bucket = a->table + h;
	struct hlist_node *tmp;
	struct arc_entry *e;

	hlist_for_each_entry(e, tmp, bucket, hlist)
		if (e->oblock == origin)
			return e;

	return NULL;
}

static void __arc_remove(struct arc_policy *a, struct arc_entry *e)
{
	hlist_del(&e->hlist);
}

static struct arc_entry *__arc_alloc_entry(struct arc_policy *a)
{
	struct arc_entry *e;

	BUG_ON(a->nr_allocated >= 2 * a->cache_size);
	e = a->entries + a->nr_allocated;
	INIT_LIST_HEAD(&e->list);
	INIT_HLIST_NODE(&e->hlist);
	a->nr_allocated++;

	return e;
}

static void __alloc_cblock(struct arc_policy *a, dm_block_t cblock)
{
	BUG_ON(cblock > a->cache_size);
	BUG_ON(test_bit(cblock, a->allocation_bitset));
	set_bit(cblock, a->allocation_bitset);
}

static void __free_cblock(struct arc_policy *a, dm_block_t cblock)
{
	BUG_ON(cblock > a->cache_size);
	BUG_ON(!test_bit(cblock, a->allocation_bitset));
	clear_bit(cblock, a->allocation_bitset);
}

/*
 * This doesn't allocate the block.
 */
static int __find_free_cblock(struct arc_policy *a, dm_block_t *result)
{
	int r = -ENOSPC;
	unsigned nr_words = dm_div_up(a->cache_size, BITS_PER_LONG);
	unsigned w, b;

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

static bool __any_free_entries(struct arc_policy *a)
{
	return a->nr_allocated < a->cache_size;
}

static void __arc_push(struct arc_policy *a,
		     enum arc_state s, struct arc_entry *e)
{
	e->state = s;

	switch (s) {
	case ARC_T1:
		__alloc_cblock(a, e->cblock);
		queue_push(&a->t1, &e->list);
		__arc_insert(a, e);
		break;

	case ARC_T2:
		__alloc_cblock(a, e->cblock);
		queue_push(&a->t2, &e->list);
		__arc_insert(a, e);
		break;

	case ARC_B1:
		queue_push(&a->b1, &e->list);
		break;

	case ARC_B2:
		queue_push(&a->b2, &e->list);
		break;
	}
}

static struct arc_entry *__arc_pop(struct arc_policy *a, enum arc_state s)
{
	struct arc_entry *e = NULL;

#define POP(x) container_of(queue_pop(x), struct arc_entry, list)

	switch (s) {
	case ARC_T1:
		BUG_ON(queue_empty(&a->t1));
		e = POP(&a->t1);
		__arc_remove(a, e);
		__free_cblock(a, e->cblock);
		break;

	case ARC_T2:
		BUG_ON(queue_empty(&a->t2));
		e = POP(&a->t2);
		__arc_remove(a, e);
		__free_cblock(a, e->cblock);
		break;

	case ARC_B1:
		BUG_ON(queue_empty(&a->b1));
		e = POP(&a->b1);
		break;

	case ARC_B2:
		BUG_ON(queue_empty(&a->b2));
		e = POP(&a->b2);
		break;
	}

#undef POP

	return e;
}

static dm_block_t __arc_demote(struct arc_policy *a, bool is_arc_b2, struct policy_result *result)
{
	struct arc_entry *e;
	dm_block_t t1_size = queue_size(&a->t1);

	result->op = POLICY_REPLACE;

	if (t1_size &&
	    ((t1_size > a->p) || (is_arc_b2 && (t1_size == a->p)))) {
		e = __arc_pop(a, ARC_T1);

		result->old_oblock = e->oblock;
		result->cblock = e->cblock;

		__arc_push(a, ARC_B1, e);
	} else {
		e = __arc_pop(a, ARC_T2);

		result->old_oblock = e->oblock;
		result->cblock = e->cblock;

		__arc_push(a, ARC_B2, e);
	}

	return e->cblock;
}

/*
 * FIXME: the size of the interesting blocks hash table seems to be
 * directly related to the eviction rate.  So maybe we should resize on the
 * fly to get to a target eviction rate?
 */
static int __arc_interesting_block(struct arc_policy *a, dm_block_t origin, int data_dir)
{
	const dm_block_t BIG_PRIME = 4294967291UL;
	unsigned h = ((unsigned) (origin * BIG_PRIME)) % a->interesting_size;

	if (origin == a->last_lookup)
		return 0;

	if (a->interesting_blocks[h] == origin)
		return 1;

	a->interesting_blocks[h] = origin;
	return 0;
}

static void __arc_map(struct arc_policy *a,
		      dm_block_t origin_block,
		      int data_dir,
		      bool can_migrate,
		      bool cheap_copy,
		      struct policy_result *result)
{
	int r;
	dm_block_t new_cache;
	dm_block_t delta;
	dm_block_t b1_size = queue_size(&a->b1);
	dm_block_t b2_size = queue_size(&a->b2);
	dm_block_t l1_size, l2_size;

	struct arc_entry *e;

	e = __arc_lookup(a, origin_block);
	if (e) {
		bool do_push = 1;

		switch (e->state) {
		case ARC_T1:
			result->op = POLICY_HIT;
			result->cblock = e->cblock;
			if (a->last_lookup != origin_block) {
				__free_cblock(a, e->cblock);
				queue_del(&a->t1, &e->list);
				__arc_remove(a, e);
			} else
				do_push = 0;
			break;

		case ARC_T2:
			result->op = POLICY_HIT;
			result->cblock = e->cblock;
			if (a->last_lookup != origin_block) {
				__free_cblock(a, e->cblock);
				queue_del(&a->t2, &e->list);
				__arc_remove(a, e);
			} else
				do_push = 0;
			break;

		case ARC_B1:
			if (!can_migrate) {
				result->op = POLICY_MISS;
				return;
			}

			delta = (b1_size > b2_size) ? 1 : max(b2_size / b1_size, 1ULL);
			a->p = min(a->p + delta, a->cache_size);
			new_cache = __arc_demote(a, 0, result);

			queue_del(&a->b1, &e->list);

			e->oblock = origin_block;
			e->cblock = new_cache;
			break;

		case ARC_B2:
			if (!can_migrate) {
				result->op = POLICY_MISS;
				return;
			}

			delta = b2_size >= b1_size ? 1 : max(b1_size / b2_size, 1ULL);
			a->p = max(a->p - delta, 0ULL);
			new_cache = __arc_demote(a, 1, result);

			queue_del(&a->b2, &e->list);

			e->oblock = origin_block;
			e->cblock = new_cache;
			break;
		}

		if (do_push)
			__arc_push(a, ARC_T2, e);
		return;
	}

	/* FIXME: this is turning into a huge mess */
	cheap_copy = cheap_copy && __any_free_entries(a);
	if (cheap_copy || (can_migrate && __arc_interesting_block(a, origin_block, data_dir))) {
		/* carry on, perverse logic */
	} else {
		result->op = POLICY_MISS;
		return;
	}

	l1_size = queue_size(&a->t1) + b1_size;
	l2_size = queue_size(&a->t2) + b2_size;
	if (l1_size == a->cache_size) {
		if (!can_migrate)  {
			result->op = POLICY_MISS;
			return;
		}

		if (queue_size(&a->t1) < a->cache_size) {
			e = __arc_pop(a, ARC_B1);

			new_cache = __arc_demote(a, 0, result);
			e->oblock = origin_block;
			e->cblock = new_cache;

		} else {
			e = __arc_pop(a, ARC_T1);

			result->op = POLICY_REPLACE;
			result->old_oblock = e->oblock;
			e->oblock = origin_block;
			result->cblock = e->cblock;
		}

	} else if (l1_size < a->cache_size && (l1_size + l2_size >= a->cache_size)) {
		if (!can_migrate)  {
			result->op = POLICY_MISS;
			return;
		}

		if (l1_size + l2_size == 2 * a->cache_size) {
			e = __arc_pop(a, ARC_B2);
			e->oblock = origin_block;
			e->cblock = __arc_demote(a, 0, result);

		} else {
			e = __arc_alloc_entry(a);
			e->oblock = origin_block;
			e->cblock = __arc_demote(a, 0, result);
			//__alloc_cblock(a, e->cblock);
		}

	} else {
		e = __arc_alloc_entry(a);
		r = __find_free_cblock(a, &e->cblock);
		BUG_ON(r);

		result->op = POLICY_NEW;
		result->cblock = e->cblock;
		e->oblock = origin_block;
	}

	__arc_push(a, ARC_T1, e);
}

static void arc_map(struct dm_cache_policy *p, dm_block_t origin_block, int data_dir,
		    bool can_migrate, bool cheap_copy, struct policy_result *result)
{
	unsigned long flags;
	struct arc_policy *a = to_arc_policy(p);

	spin_lock_irqsave(&a->lock, flags);
	__arc_map(a, origin_block, data_dir, can_migrate, cheap_copy, result);
	a->last_lookup = origin_block;
	spin_unlock_irqrestore(&a->lock, flags);
}

static int arc_load_mapping(struct dm_cache_policy *p, dm_block_t oblock, dm_block_t cblock)
{
	struct arc_policy *a = to_arc_policy(p);
	struct arc_entry *e;

	debug("loading mapping %lu -> %lu, context = %p\n",
	      (unsigned long) oblock,
	      (unsigned long) cblock,
	      context);

	e = __arc_alloc_entry(a);
	if (!e)
		return -ENOMEM;

	e->cblock = cblock;
	e->oblock = oblock;
	__arc_push(a, ARC_T1, e);

	return 0;
}

static dm_block_t arc_residency(struct dm_cache_policy *p)
{
	struct arc_policy *a = to_arc_policy(p);
	return min(a->nr_allocated, a->cache_size);
}

/*----------------------------------------------------------------*/

struct dm_cache_policy *arc_policy_create(dm_block_t cache_size)
{
	dm_block_t nr_buckets;
	struct arc_policy *a = kmalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return NULL;

	a->policy.destroy = arc_destroy;
	a->policy.map = arc_map;
	a->policy.load_mapping = arc_load_mapping;
	a->policy.residency = arc_residency;

	a->cache_size = cache_size;
	spin_lock_init(&a->lock);
	a->p = 0;

	queue_init(&a->b1);
	queue_init(&a->t1);
	queue_init(&a->b2);
	queue_init(&a->t2);

	a->entries = vmalloc(sizeof(*a->entries) * 2 * cache_size);
	if (!a->entries) {
		kfree(a);
		return NULL;
	}

	a->nr_allocated = 0;

	a->nr_buckets = cache_size / 8;
	nr_buckets = 16;
	while (nr_buckets < a->nr_buckets)
		nr_buckets <<= 1;
	a->nr_buckets = nr_buckets;

	a->hash_mask = a->nr_buckets - 1;
	a->table = kzalloc(sizeof(*a->table) * a->nr_buckets, GFP_KERNEL);
	if (!a->table) {
		vfree(a->entries);
		kfree(a);
		return NULL;
	}

	a->interesting_size = cache_size / 2;
	a->interesting_blocks = vzalloc(sizeof(*a->interesting_blocks) * a->interesting_size);
	if (!a->interesting_blocks) {
		kfree(a->table);
		vfree(a->entries);
		kfree(a);
		return NULL;
	}

	a->allocation_bitset = alloc_bitset(cache_size, 0);
	if (!a->allocation_bitset) {
		vfree(a->interesting_blocks);
		kfree(a->table);
		vfree(a->entries);
		kfree(a);
		return NULL;
	}

	return &a->policy;
}

/*----------------------------------------------------------------*/
