/*
 * Copyright (C) 2012 Red Hat. All rights reserved.
 *
 * writeback cache policy supporting flushing out dirty cache blocks.
 *
 * This file is released under the GPL.
 */

#include "dm-cache-policy.h"
#include "dm.h"

#include <linux/hash.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>

/*----------------------------------------------------------------*/

/* Cache entry struct. */
struct wb_cache_entry {
	struct list_head list;
	struct hlist_node hlist;

	dm_cblock_t cblock;
	dm_oblock_t oblock;
	bool dirty;
};

struct hash {
	struct hlist_head *table;
	dm_block_t hash_bits;
	unsigned nr_buckets;
};

struct policy {
	struct dm_cache_policy policy;
	struct mutex lock;

	struct list_head free, used; /* Free/used cache entry list */

	/*
	 * We know exactly how many cblocks will be needed,
	 * so we can allocate them up front.
	 */
	dm_cblock_t cache_size, nr_cblocks_allocated;
	struct wb_cache_entry *cblocks;
	struct hash chash;

	dm_block_t nr_dirty;
};

/*----------------------------------------------------------------------------*/
/* Low-level functions. */
static unsigned next_power(unsigned n, unsigned min)
{
	return roundup_pow_of_two(max(n, min));
}

static struct policy *to_policy(struct dm_cache_policy *p)
{
	return container_of(p, struct policy, policy);
}

static struct list_head *list_pop(struct list_head *q)
{
	struct list_head *r = q->next;

	BUG_ON(!r);
	list_del(r);

	return r;
}

/*----------------------------------------------------------------------------*/

/* Allocate/free various resources. */
static int alloc_hash(struct hash *hash, unsigned elts)
{
	hash->nr_buckets = next_power(elts >> 4, 16);
	hash->hash_bits = ffs(hash->nr_buckets) - 1;
	hash->table = vzalloc(sizeof(*hash->table) * hash->nr_buckets);

	return hash->table ? 0 : -ENOMEM;
}

static void free_hash(struct hash *hash)
{
	vfree(hash->table);
}

static int alloc_cache_blocks_with_hash(struct policy *p, dm_cblock_t cache_size)
{
	int r;

	p->cblocks = vzalloc(sizeof(*p->cblocks) * from_cblock(cache_size));
	if (p->cblocks) {
		unsigned u = from_cblock(cache_size);

		while (u--)
			list_add(&p->cblocks[u].list, &p->free);

		p->nr_cblocks_allocated = 0;

		/* Cache entries hash. */
		r = alloc_hash(&p->chash, from_cblock(cache_size));
		if (r)
			vfree(p->cblocks);

	} else
		r = -ENOMEM;

	return r;
}

static void free_cache_blocks_and_hash(struct policy *p)
{
	free_hash(&p->chash);
	vfree(p->cblocks);
}

static struct wb_cache_entry *alloc_cache_entry(struct policy *p)
{
	struct wb_cache_entry *e;

	BUG_ON(from_cblock(p->nr_cblocks_allocated) >= from_cblock(p->cache_size));

	e = list_entry(list_pop(&p->free), struct wb_cache_entry, list);
	p->nr_cblocks_allocated = to_cblock(from_cblock(p->nr_cblocks_allocated) + 1);

	return e;
}

/*----------------------------------------------------------------------------*/

/* Hash functions (lookup, insert, remove). */
static struct wb_cache_entry *lookup_cache_entry(struct policy *p, dm_oblock_t oblock)
{
	struct hash *hash = &p->chash;
	unsigned h = hash_64(from_oblock(oblock), hash->hash_bits);
	struct wb_cache_entry *cur;
	struct hlist_node *tmp;
	struct hlist_head *bucket = &hash->table[h];

	hlist_for_each_entry(cur, tmp, bucket, hlist) {
		if (cur->oblock == oblock) {
			/* Move upfront bucket for faster access. */
			hlist_del(&cur->hlist);
			hlist_add_head(&cur->hlist, bucket);
			return cur;
		}
	}

	return NULL;
}

static void insert_cache_hash_entry(struct policy *p, struct wb_cache_entry *e)
{
	unsigned h = hash_64(from_oblock(e->oblock), p->chash.hash_bits);

	hlist_add_head(&e->hlist, &p->chash.table[h]);
}

static void remove_cache_hash_entry(struct wb_cache_entry *e)
{
	hlist_del(&e->hlist);
}

/* Public interface (see dm-cache-policy.h */
static int wb_map(struct dm_cache_policy *pe, dm_oblock_t oblock,
		     bool can_migrate, bool discarded_oblock,
		     struct bio *bio,
		     struct policy_result *result)
{
	struct policy *p = to_policy(pe);
	struct wb_cache_entry *e;

	if (can_migrate)
		mutex_lock(&p->lock);

	else if (!mutex_trylock(&p->lock))
		return -EWOULDBLOCK;

	e = lookup_cache_entry(p, oblock);
	if (e) {
		result->op = POLICY_HIT;
		result->cblock = e->cblock;

	} else
		result->op = POLICY_MISS;

	mutex_unlock(&p->lock);

	return 0;
}

static void __set_clear_dirty(struct dm_cache_policy *pe, dm_cblock_t cblock, bool set)
{
	struct policy *p = to_policy(pe);
	struct wb_cache_entry *e;

	e = lookup_cache_entry(p, cblock);
	BUG_ON(!e);

	if (set) {
		if (!e->dirty) {
			e->dirty = true;
			p->nr_dirty++;
		}

	} else {
		if (e->dirty) {
			e->dirty = false;
			BUG_ON(!p->nr_dirty);
			p->nr_dirty--;
		}
	}
}

static void wb_set_dirty(struct dm_cache_policy *pe, dm_cblock_t cblock)
{
	__set_clear_dirty(pe, cblock, true);
}

static void wb_clear_dirty(struct dm_cache_policy *pe, dm_cblock_t cblock)
{
	__set_clear_dirty(pe, cblock, false);
}

static void add_cache_entry(struct policy *p, struct wb_cache_entry *e)
{
	insert_cache_hash_entry(p, e);
	list_add(&e->list, &p->used);
}

static int wb_load_mapping(struct dm_cache_policy *pe,
			   dm_oblock_t oblock, dm_cblock_t cblock,
			   uint32_t hint, bool hint_valid)
{
	int r;
	struct policy *p = to_policy(pe);
	struct wb_cache_entry *e = alloc_cache_entry(p);

	if (e) {
		e->cblock = cblock;
		e->oblock = oblock;
		add_cache_entry(p, e);
		r = 0;

	} else
		r = -ENOMEM;

	return r;
}

static void wb_destroy(struct dm_cache_policy *pe)
{
	struct policy *p = to_policy(pe);

	free_cache_blocks_and_hash(p);
	kfree(p);
}

static struct wb_cache_entry *__wb_force_remove_mapping(struct policy *p, dm_oblock_t oblock)
{
	struct wb_cache_entry *r = lookup_cache_entry(p, oblock);

	BUG_ON(!r);

	remove_cache_hash_entry(r);
	list_del(&r->list);

	return r;
}

static void wb_remove_mapping(struct dm_cache_policy *pe, dm_oblock_t oblock)
{
	struct policy *p = to_policy(pe);
	struct wb_cache_entry *e;

	mutex_lock(&p->lock);
	e = __wb_force_remove_mapping(p, oblock);
	list_add_tail(&e->list, &p->free);
	BUG_ON(!from_cblock(p->nr_cblocks_allocated));
	p->nr_cblocks_allocated = to_cblock(from_cblock(p->nr_cblocks_allocated) - 1);
	mutex_unlock(&p->lock);
}

static void wb_force_mapping(struct dm_cache_policy *pe,
				dm_oblock_t current_oblock, dm_oblock_t oblock)
{
	struct policy *p = to_policy(pe);
	struct wb_cache_entry *e;

	mutex_lock(&p->lock);
	e = __wb_force_remove_mapping(p, current_oblock);
	e->oblock = oblock;
	add_cache_entry(p, e);
	mutex_unlock(&p->lock);
}

static struct wb_cache_entry *get_next_dirty_entry(struct policy *p)
{
	struct wb_cache_entry *r;

	BUG_ON(p->nr_dirty >= p->cache_size);

	if (!p->nr_dirty)
		return NULL;

	list_for_each_entry(r, &p->used, list) {
		if (r->dirty) {
			list_del_init(&p->used);
			list_splice(&r->list, &p->used);
			break;
		}
	}

	BUG_ON(!r->dirty);
	r->dirty = false;
	BUG_ON(!p->nr_dirty);
	p->nr_dirty--;

	return r;
}

static int wb_writeback_work(struct dm_cache_policy *pe,
			     dm_oblock_t *oblock,
			     dm_cblock_t *cblock)
{
	int r;
	struct policy *p = to_policy(pe);
	struct wb_cache_entry *e;

	mutex_lock(&p->lock);

	e = get_next_dirty_entry(p);
	if (e) {
		*oblock = e->oblock;
		*cblock = e->cblock;
		r = 0;

	} else
		r = -ENOENT;

	mutex_unlock(&p->lock);

	return r;
}

static dm_cblock_t wb_residency(struct dm_cache_policy *pe)
{
	struct policy *p = to_policy(pe);
	dm_cblock_t r;

	mutex_lock(&p->lock);
	r = from_cblock(p->nr_cblocks_allocated);
	mutex_unlock(&p->lock);

	return r;
}

static int wb_status(struct dm_cache_policy *pe, status_type_t type, unsigned status_flags, char *result, unsigned maxlen)
{
	ssize_t sz = 0;
	struct policy *p = to_policy(pe);

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%llu", (long long unsigned) p->nr_dirty);
		break;

	case STATUSTYPE_TABLE:
		break;
	}

	return 0;
}

/* Init the policy plugin interface function pointers. */
static void init_policy_functions(struct policy *p)
{
	p->policy.destroy = wb_destroy;
	p->policy.map = wb_map;
	p->policy.set_dirty = wb_set_dirty;
	p->policy.clear_dirty = wb_clear_dirty;
	p->policy.load_mapping = wb_load_mapping;
	p->policy.load_mappings_completed = NULL;
	p->policy.walk_mappings = NULL;
	p->policy.remove_mapping = wb_remove_mapping;
	p->policy.force_mapping = wb_force_mapping;
	p->policy.writeback_work = wb_writeback_work;
	p->policy.residency = wb_residency;
	p->policy.tick = NULL;
	p->policy.status = wb_status;
	p->policy.message = NULL;
}

static struct dm_cache_policy *wb_create(dm_cblock_t cache_size,
					 sector_t origin_size,
					 sector_t block_size,
					 int argc, char **argv)
{
	int r;
	struct policy *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return NULL;

	init_policy_functions(p);
	INIT_LIST_HEAD(&p->free);
	INIT_LIST_HEAD(&p->used);
	p->cache_size = cache_size;
	mutex_init(&p->lock);

	/* Allocate cache entry structs and add them to free list. */
	r = alloc_cache_blocks_with_hash(p, cache_size);
	if (!r)
		return &p->policy;

	kfree(p);

	return NULL;
}
/*----------------------------------------------------------------------------*/

static struct dm_cache_policy_type wb_policy_type = {
	.name = "writeback",
	.hint_size = 0,
	.owner = THIS_MODULE,
        .create = wb_create
};

static int __init wb_init(void)
{
	return dm_cache_policy_register(&wb_policy_type);
}

static void __exit wb_exit(void)
{
	dm_cache_policy_unregister(&wb_policy_type);
}

module_init(wb_init);
module_exit(wb_exit);

MODULE_AUTHOR("Heinz Mauelshagen");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("writeback cache policy");

/*----------------------------------------------------------------------------*/
