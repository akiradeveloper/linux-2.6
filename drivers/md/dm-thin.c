/*
 * Copyright (C) 2011 Red Hat UK.  All rights reserved.
 *
 * This file is released under the GPL.
 */

#include "dm-thin-metadata.h"
#include "persistent-data/dm-transaction-manager.h"

#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/dm-io.h>
#include <linux/dm-kcopyd.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#define	DM_MSG_PREFIX	"thin"

/*
 * How do we handle breaking sharing of data blocks?
 * =================================================
 *
 * We use a standard copy-on-write btree to store the mappings for the
 * devices (note I'm talking about copy-on-write of the metadata here, not
 * the data).  When you take an internal snapshot you clone the root node
 * of the origin btree.  After this there is no concept of an origin or a
 * snapshot.  They are just two device trees that happen to point to the
 * same data blocks.
 *
 * When we get a write in we decide if it's to a shared data block using
 * some timestamp magic.  If it is, we have to break sharing.
 *
 * Let's say we write to a shared block in what was the origin.  The
 * steps are:
 *
 * i) plug io further to this physical block. (see bio_prison code).
 *
 * ii) quiesce any read io to that shared data block.  Obviously
 * including all devices that share this block.  (see deferred_set code)
 *
 * iii) copy the data block to a newly allocate block.  This step can be
 * missed out if the io covers the block. (schedule_copy).
 *
 * iv) insert the new mapping into the origin's btree
 * (process_prepared_mappings).  This act of inserting breaks some
 * sharing of btree nodes between the two devices.  Breaking sharing only
 * effects the btree of that specific device.  Btrees for the other
 * devices that share the block never change.  The btree for the origin
 * device as it was after the last commit is untouched, ie. we're using
 * persistent data structures in the functional programming sense.
 *
 * v) unplug io to this physical block, including the io that triggered
 * the breaking of sharing.
 *
 * Steps (ii) and (iii) occur in parallel.
 *
 * The metadata _doesn't_ need to be committed before the io continues.  We
 * get away with this because the io is always written to a _new_ block.
 * If there's a crash, then:
 *
 * - The origin mapping will point to the old origin block (the shared
 * one).  This will contain the data as it was before the io that triggered
 * the breaking of sharing came in.
 *
 * - The snap mapping still points to the old block.  As it would after
 * the commit.
 *
 * The downside of this scheme is the timestamp magic isn't perfect, and
 * will continue to think that data block in the snapshot device is shared
 * even after the write to the origin has broken sharing.  I suspect data
 * blocks will typically be shared by many different devices, so we're
 * breaking sharing n + 1 times, rather than n, where n is the number of
 * devices that reference this data block.  At the moment I think the
 * benefits far, far outweigh the disadvantages.
 */

/*----------------------------------------------------------------*/

/*
 * Function that breaks abstraction layering.
 */
static struct block_device *get_target_bdev(struct dm_target *ti)
{
	return dm_bdev(dm_table_get_md(ti->table));
}

/*----------------------------------------------------------------*/

/*
 * Sometimes we can't deal with a bio straight away.  We put them in prison
 * where they can't cause any mischief.  Bios are put in a cell identified
 * by a key, multiple bios can be in the same cell.  When the cell is
 * subsequently unlocked the bios become available.
 */
struct bio_prison;

struct cell_key {
	int virtual;
	dm_thin_dev_t dev;
	dm_block_t block;
};

struct cell {
	struct hlist_node list;
	struct bio_prison *prison;
	struct cell_key key;
	unsigned count;
	struct bio_list bios;
};

struct bio_prison {
	spinlock_t lock;
	mempool_t *cell_pool;

	unsigned nr_buckets;
	unsigned hash_mask;
	struct hlist_head *cells;
};

static uint32_t calc_nr_buckets(unsigned nr_cells)
{
	uint32_t n = 128;
	nr_cells /= 4;
	nr_cells = min(nr_cells, 8192u);
	while (n < nr_cells)
		n <<= 1;

	return n;
}

/*
 * @nr_cells should be the number of cells you want in use _concurrently_.
 * Don't confuse it with the number of distinct keys.
 */
static struct bio_prison *prison_create(unsigned nr_cells)
{
	int i;
	uint32_t nr_buckets = calc_nr_buckets(nr_cells);
	size_t len = sizeof(struct bio_prison) +
		(sizeof(struct hlist_head) * nr_buckets);
	struct bio_prison *prison = kmalloc(len, GFP_KERNEL);
	if (!prison)
		return NULL;

	spin_lock_init(&prison->lock);
	prison->cell_pool = mempool_create_kmalloc_pool(nr_cells,
							sizeof(struct cell));
	prison->nr_buckets = nr_buckets;
	prison->hash_mask = nr_buckets - 1;
	prison->cells = (struct hlist_head *) (prison + 1);
	for (i = 0; i < nr_buckets; i++)
		INIT_HLIST_HEAD(prison->cells + i);

	return prison;
}

static void prison_destroy(struct bio_prison *prison)
{
	mempool_destroy(prison->cell_pool);
	kfree(prison);
}

static uint32_t hash_key(struct bio_prison *prison, struct cell_key *key)
{
	const unsigned BIG_PRIME = 4294967291UL;
	uint64_t hash = key->block * BIG_PRIME;
	return (uint32_t) (hash & prison->hash_mask);
}

static struct cell *__search_bucket(struct hlist_head *bucket,
				    struct cell_key *key)
{
	struct cell *cell;
	struct hlist_node *tmp;

	hlist_for_each_entry(cell, tmp, bucket, list)
		if (!memcmp(&cell->key, key, sizeof(cell->key)))
			return cell;

	return NULL;
}

/*
 * This may block if a new cell needs allocating.  You must ensure that
 * cells will be unlocked even if the calling thread is blocked.
 *
 * returns the number of entries in the cell prior to the new addition. or
 * < 0 on failure.
 */
static int bio_detain(struct bio_prison *prison, struct cell_key *key,
		      struct bio *inmate, struct cell **ref)
{
	int r;
	unsigned long flags;
	uint32_t hash = hash_key(prison, key);
	struct cell *uninitialized_var(cell), *cell2 = NULL;

	BUG_ON(hash > prison->nr_buckets);

	spin_lock_irqsave(&prison->lock, flags);
	cell = __search_bucket(prison->cells + hash, key);

	if (!cell) {
		/* allocate a new cell */
		spin_unlock_irqrestore(&prison->lock, flags);
		cell2 = mempool_alloc(prison->cell_pool, GFP_NOIO);
		spin_lock_irqsave(&prison->lock, flags);

		/*
		 * We've been unlocked, so we have to double check that
		 * nobody else has inserted this cell in the mean time.
		 */
		cell = __search_bucket(prison->cells + hash, key);

		if (!cell) {
			cell = cell2;
			cell2 = NULL;

			cell->prison = prison;
			memcpy(&cell->key, key, sizeof(cell->key));
			cell->count = 0;
			bio_list_init(&cell->bios);
			hlist_add_head(&cell->list, prison->cells + hash);
		}
	}

	r = cell->count++;
	bio_list_add(&cell->bios, inmate);
	spin_unlock_irqrestore(&prison->lock, flags);

	if (cell2)
		mempool_free(cell2, prison->cell_pool);

	*ref = cell;
	return r;
}

static int bio_detain_if_occupied(struct bio_prison *prison, struct cell_key *key,
				  struct bio *inmate, struct cell **ref)
{
	int r;
	unsigned long flags;
	uint32_t hash = hash_key(prison, key);
	struct cell *uninitialized_var(cell);

	BUG_ON(hash > prison->nr_buckets);

	spin_lock_irqsave(&prison->lock, flags);
	cell = __search_bucket(prison->cells + hash, key);

	if (!cell) {
		spin_unlock_irqrestore(&prison->lock, flags);
		return 0;
	}

	r = cell->count++;
	bio_list_add(&cell->bios, inmate);
	spin_unlock_irqrestore(&prison->lock, flags);

	*ref = cell;
	return r;
}

/* @inmates must have been initialised prior to this call */
static void __cell_release(struct cell *cell, struct bio_list *inmates)
{
	struct bio_prison *prison = cell->prison;
	hlist_del(&cell->list);
	if (inmates)
		bio_list_merge(inmates, &cell->bios);
	mempool_free(cell, prison->cell_pool);
}

static void cell_release(struct cell *cell, struct bio_list *bios)
{
	unsigned long flags;
	struct bio_prison *prison = cell->prison;

	spin_lock_irqsave(&prison->lock, flags);
	__cell_release(cell, bios);
	spin_unlock_irqrestore(&prison->lock, flags);
}

static void cell_error(struct cell *cell)
{
	struct bio_prison *prison = cell->prison;
	struct bio_list bios;
	struct bio *bio;
	unsigned long flags;

	bio_list_init(&bios);

	spin_lock_irqsave(&prison->lock, flags);
	__cell_release(cell, &bios);
	spin_unlock_irqrestore(&prison->lock, flags);

	while ((bio = bio_list_pop(&bios)))
		bio_io_error(bio);
}

/*----------------------------------------------------------------*/

/*
 * We use the deferred set to keep track of pending reads to shared blocks.
 * We do this to ensure the new mapping caused by a write isn't performed
 * until these prior reads have completed.  Otherwise the insertion of the
 * new mapping could free the old block that the read bios are mapped to.
 */
#define DEFERRED_SET_SIZE 64

struct deferred_set;
struct deferred_entry {
	struct deferred_set *ds;
	unsigned count;
	struct list_head work_items;
};

struct deferred_set {
	spinlock_t lock;
	unsigned current_entry;
	unsigned sweeper;
	struct deferred_entry entries[DEFERRED_SET_SIZE];
};

static void ds_init(struct deferred_set *ds)
{
	int i;

	spin_lock_init(&ds->lock);
	ds->current_entry = 0;
	ds->sweeper = 0;
	for (i = 0; i < DEFERRED_SET_SIZE; i++) {
		ds->entries[i].ds = ds;
		ds->entries[i].count = 0;
		INIT_LIST_HEAD(&ds->entries[i].work_items);
	}
}

static struct deferred_entry *ds_inc(struct deferred_set *ds)
{
	unsigned long flags;
	struct deferred_entry *entry;

	spin_lock_irqsave(&ds->lock, flags);
	entry = ds->entries + ds->current_entry;
	entry->count++;
	spin_unlock_irqrestore(&ds->lock, flags);

	return entry;
}

static unsigned ds_next(unsigned index)
{
	return (index + 1) % DEFERRED_SET_SIZE;
}

static void __sweep(struct deferred_set *ds, struct list_head *head)
{
	while ((ds->sweeper != ds->current_entry) &&
	       !ds->entries[ds->sweeper].count) {
		list_splice_init(&ds->entries[ds->sweeper].work_items, head);
		ds->sweeper = ds_next(ds->sweeper);
	}

	if ((ds->sweeper == ds->current_entry) && !ds->entries[ds->sweeper].count)
		list_splice_init(&ds->entries[ds->sweeper].work_items, head);
}

static void ds_dec(struct deferred_entry *entry, struct list_head *head)
{
	unsigned long flags;

	spin_lock_irqsave(&entry->ds->lock, flags);
	BUG_ON(!entry->count);
	--entry->count;
	__sweep(entry->ds, head);
	spin_unlock_irqrestore(&entry->ds->lock, flags);
}

/* 1 if deferred, 0 if no pending items to delay job */
static int ds_add_work(struct deferred_set *ds, struct list_head *work)
{
	int r = 1;
	unsigned long flags;
	unsigned next_entry;

	spin_lock_irqsave(&ds->lock, flags);
	if ((ds->sweeper == ds->current_entry) &&
	    !ds->entries[ds->current_entry].count)
		r = 0;
	else {
		list_add(work, &ds->entries[ds->current_entry].work_items);
		next_entry = ds_next(ds->current_entry);
		if (!ds->entries[next_entry].count) {
			BUG_ON(!list_empty(&ds->entries[next_entry].work_items));
			ds->current_entry = next_entry;
		}
	}
	spin_unlock_irqrestore(&ds->lock, flags);

	return r;
}

/*----------------------------------------------------------------*/

/*
 * Key building.
 */
static void build_data_key(struct dm_thin_device *td,
			   dm_block_t b, struct cell_key *key)
{
	key->virtual = 0;
	key->dev = dm_thin_device_dev(td);
	key->block = b;
}

static void build_virtual_key(struct dm_thin_device *td, dm_block_t b,
			      struct cell_key *key)
{
	key->virtual = 1;
	key->dev = dm_thin_device_dev(td);
	key->block = b;
}

/*----------------------------------------------------------------*/

/*
 * A pool device ties together a metadata device and a data device.  It
 * also provides the interface for creating and destroying internal
 * devices.
 */
struct pool {
	struct hlist_node hlist;
	struct dm_target *ti;	/* only set if a pool target is bound */

	struct block_device *pool_dev;
	struct dm_thin_metadata *tmd;

	uint32_t sectors_per_block;
	unsigned block_shift;
	dm_block_t offset_mask;
	dm_block_t low_water_mark;
	unsigned zero_new_blocks;

	struct bio_prison *prison;
	struct dm_kcopyd_client *copier;

	struct workqueue_struct *producer_wq;
	struct workqueue_struct *consumer_wq;
	struct work_struct producer;
	struct work_struct consumer;

	spinlock_t lock;
	struct bio_list deferred_bios;
	struct list_head prepared_mappings;

	int triggered;		/* a dm event has been sent */
	struct bio_list retry_list;

	struct deferred_set ds;	/* FIXME: move to thin_c */

	mempool_t *mapping_pool;
	mempool_t *endio_hook_pool;

	atomic_t ref_count;
};

/*
 * Target context for a pool.
 */
struct pool_c {
	struct dm_target *ti;
	struct pool *pool;
	struct dm_dev *data_dev;
	struct dm_dev *metadata_dev;
	struct dm_target_callbacks callbacks;

	sector_t low_water_mark;
	unsigned zero_new_blocks;
};

/*
 * Target context for a thin.
 */
struct thin_c {
	struct dm_dev *pool_dev;
	dm_thin_dev_t dev_id;

	/*
	 * These fields are only valid while the device is resumed.  This
	 * is because the pool_c may totally change due to table reloads
	 * (where as the pool_dev above remains constant).
	 */
	struct pool *pool;
	struct dm_thin_device *td;
};

struct endio_hook {
	struct thin_c *tc;
	bio_end_io_t *bi_end_io;
	void *bi_private;
	struct deferred_entry *entry;
};

/* FIXME: can cells and new_mappings be combined? */

struct new_mapping {
	struct list_head list;

	int prepared;

	struct thin_c *tc;
	dm_block_t virt_block;
	dm_block_t data_block;
	struct cell *cell;
	int err;

	/*
	 * If the bio covers the whole area of a block then we can avoid
	 * zeroing or copying.  Instead this bio is hooked.  The bio will
	 * still be in the cell, so care has to be taken to avoid issuing
	 * the bio twice.
	 */
	struct bio *bio;
	bio_end_io_t *bi_end_io;
	void *bi_private;
};

/*----------------------------------------------------------------*/

/*
 * A global table that uses a struct block_device as a key.
 */
#define TABLE_SIZE 32
#define TABLE_PRIME 27 /* Largest prime smaller than table size. */
#define	TABLE_SHIFT 5  /* Shift fitting prime. */

static struct dm_thin_pool_table {
	spinlock_t lock;
	struct hlist_head buckets[TABLE_SIZE];
} dm_thin_pool_table;

static void pool_table_init(void)
{
	unsigned i;

	spin_lock_init(&dm_thin_pool_table.lock);
	for (i = 0; i < TABLE_SIZE; i++)
		INIT_HLIST_HEAD(dm_thin_pool_table.buckets + i);
}

static unsigned hash_bdev(struct block_device *bdev)
{
	unsigned long p = (unsigned long) bdev;

	do_div(p, cache_line_size());
	return ((p * TABLE_PRIME) >> TABLE_SHIFT) & (TABLE_SIZE - 1);
}

static void pool_table_insert(struct pool *pool)
{
	unsigned bucket = hash_bdev(pool->pool_dev);

	spin_lock(&dm_thin_pool_table.lock);
	hlist_add_head(&pool->hlist, dm_thin_pool_table.buckets + bucket);
	spin_unlock(&dm_thin_pool_table.lock);
}

static void pool_table_remove(struct pool *pool)
{
	spin_lock(&dm_thin_pool_table.lock);
	hlist_del(&pool->hlist);
	spin_unlock(&dm_thin_pool_table.lock);
}

static struct pool *pool_table_lookup(struct block_device *bdev)
{
	unsigned bucket = hash_bdev(bdev);
	struct hlist_node *n;
	struct pool *pool;

	hlist_for_each_entry(pool, n, dm_thin_pool_table.buckets + bucket, hlist)
		if (pool->pool_dev == bdev)
			return pool;

	return NULL;
}

/*----------------------------------------------------------------*/

/*
 * We need to maintain an association between a bio and a thin target. To
 * save lookups in an auxillary table, or wrapping bios in objects from a
 * mempool we hide this value in the bio->bi_bdev field, which we know is
 * not used while the bio is being processed.
 */
static void set_tc(struct bio *bio, struct thin_c *tc)
{
	bio->bi_bdev = (struct block_device *)tc;
}

static struct thin_c *get_tc(struct bio *bio)
{
	return (struct thin_c *)bio->bi_bdev;
}

/*----------------------------------------------------------------*/

/*
 * This section of code contains the logic for processing a thin devices' IO.
 * Much of the code depends on pool object resources (lists, workqueues, etc)
 * but most is exclusively called from the thin target rather than the thin-pool
 * target.  wake_producer() being the most notable exception (which is also used
 * by thin-pool to continue deferred IO processing after pool resume).
 */

static dm_block_t get_bio_block(struct thin_c *tc, struct bio *bio)
{
	return bio->bi_sector >> tc->pool->block_shift;
}

static void remap(struct thin_c *tc, struct bio *bio, dm_block_t block)
{
	struct pool *pool = tc->pool;

	bio->bi_bdev = tc->pool_dev->bdev;
	bio->bi_sector = (block << pool->block_shift) +
		(bio->bi_sector & pool->offset_mask);
}

static void remap_and_issue(struct thin_c *tc, struct bio *bio,
			    dm_block_t block)
{
	if (bio->bi_rw & (REQ_FLUSH | REQ_FUA)) {
		int r = dm_thin_metadata_commit(tc->pool->tmd);
		if (r) {
			DMERR("%s: dm_thin_metadata_commit() failed, error = %d",
			      __func__, r);
			bio_io_error(bio);
			return;
		}
	}

	remap(tc, bio, block);
	generic_make_request(bio);
}

static void wake_producer(struct pool *pool)
{
	queue_work(pool->producer_wq, &pool->producer);
}

static void __maybe_add_mapping(struct new_mapping *m)
{
	struct pool *pool = m->tc->pool;

	if (list_empty(&m->list) && m->prepared) {
		list_add(&m->list, &pool->prepared_mappings);
		queue_work(pool->consumer_wq, &pool->consumer);
	}
}

static void copy_complete(int read_err, unsigned long write_err, void *context)
{
	unsigned long flags;
	struct new_mapping *m = (struct new_mapping *)context;
	struct pool *pool = m->tc->pool;

	m->err = read_err || write_err ? -EIO : 0;

	spin_lock_irqsave(&pool->lock, flags);
	m->prepared = 1;
	__maybe_add_mapping(m);
	spin_unlock_irqrestore(&pool->lock, flags);
}

static void overwrite_complete(struct bio *bio, int err)
{
	unsigned long flags;
	struct new_mapping *m = (struct new_mapping *)bio->bi_private;
	struct pool *pool = m->tc->pool;

	m->err = err;

	spin_lock_irqsave(&pool->lock, flags);
	m->prepared = 1;
	__maybe_add_mapping(m);
	spin_unlock_irqrestore(&pool->lock, flags);
}

static void shared_read_complete(struct bio *bio, int err)
{
	struct list_head mappings;
	struct new_mapping *m, *tmp;
	struct endio_hook *h = (struct endio_hook *)bio->bi_private;
	unsigned long flags;

	bio->bi_end_io = h->bi_end_io;
	bio->bi_private = h->bi_private;
	bio_endio(bio, err);

	INIT_LIST_HEAD(&mappings);
	ds_dec(h->entry, &mappings);

	spin_lock_irqsave(&h->tc->pool->lock, flags);
	list_for_each_entry_safe(m, tmp, &mappings, list) {
		list_del(&m->list);
		INIT_LIST_HEAD(&m->list);
		__maybe_add_mapping(m);
	}
	spin_unlock_irqrestore(&h->tc->pool->lock, flags);

	mempool_free(h, h->tc->pool->endio_hook_pool);
}

static int io_covers_block(struct pool *pool, struct bio *bio)
{
	return ((bio->bi_sector & pool->offset_mask) == 0) &&
		(bio->bi_size == (pool->sectors_per_block << SECTOR_SHIFT));
}

static void schedule_copy(struct thin_c *tc, dm_block_t virt_block,
			  dm_block_t data_origin, dm_block_t data_dest,
			  struct cell *cell, struct bio *bio)
{
	int r;
	struct pool *pool = tc->pool;
	struct new_mapping *m = mempool_alloc(pool->mapping_pool, GFP_NOIO);

	INIT_LIST_HEAD(&m->list);
	m->prepared = 0;
	m->tc = tc;
	m->virt_block = virt_block;
	m->data_block = data_dest;
	m->cell = cell;
	m->err = 0;
	m->bio = NULL;

	ds_add_work(&pool->ds, &m->list);

	if (io_covers_block(pool, bio)) {
		/* no copy needed, since all data is going to change */
		m->bio = bio;
		m->bi_end_io = bio->bi_end_io;
		m->bi_private = bio->bi_private;
		bio->bi_end_io = overwrite_complete;
		bio->bi_private = m;
		remap_and_issue(tc, bio, data_dest);

	} else {
		/* use kcopyd */
		struct dm_io_region from, to;

		/* IO to pool_dev remaps to the pool target's data_dev */
		from.bdev = tc->pool_dev->bdev;
		from.sector = data_origin * pool->sectors_per_block;
		from.count = pool->sectors_per_block;

		to.bdev = tc->pool_dev->bdev;
		to.sector = data_dest * pool->sectors_per_block;
		to.count = pool->sectors_per_block;

		r = dm_kcopyd_copy(pool->copier, &from, 1, &to,
				   0, copy_complete, m);
		if (r < 0) {
			mempool_free(m, pool->mapping_pool);
			DMERR("dm_kcopyd_copy() failed");
			cell_error(cell);
		}
	}
}

static void schedule_zero(struct thin_c *tc, dm_block_t virt_block,
			  dm_block_t data_block, struct cell *cell,
			  struct bio *bio)
{
	struct pool *pool = tc->pool;
	struct new_mapping *m = mempool_alloc(pool->mapping_pool, GFP_NOIO);

	INIT_LIST_HEAD(&m->list);
	m->prepared = 0;
	m->tc = tc;
	m->virt_block = virt_block;
	m->data_block = data_block;
	m->cell = cell;
	m->err = 0;
	m->bio = NULL;

	if (!pool->zero_new_blocks || io_covers_block(pool, bio)) {
		/* no copy needed, since all data is going to change */
		m->bio = bio;
		m->bi_end_io = bio->bi_end_io;
		m->bi_private = bio->bi_private;
		bio->bi_end_io = overwrite_complete;
		bio->bi_private = m;
		remap_and_issue(tc, bio, data_block);

	} else {
		int r;
		struct dm_io_region to;

		to.bdev = tc->pool_dev->bdev;
		to.sector = data_block * pool->sectors_per_block;
		to.count = pool->sectors_per_block;

		r = dm_kcopyd_zero(pool->copier, 1, &to, 0, copy_complete, m);
		if (r < 0) {
			mempool_free(m, pool->mapping_pool);
			DMERR("dm_kcopyd_zero() failed");
			cell_error(cell);
		}
	}
}

static void cell_remap_and_issue(struct thin_c *tc, struct cell *cell,
				 dm_block_t data_block)
{
	struct bio_list bios;
	struct bio *bio;

	bio_list_init(&bios);
	cell_release(cell, &bios);

	while ((bio = bio_list_pop(&bios)))
		remap_and_issue(tc, bio, data_block);
}

static void cell_remap_and_issue_except(struct thin_c *tc, struct cell *cell,
					dm_block_t data_block,
					struct bio *exception)
{
	struct bio_list bios;
	struct bio *bio;

	bio_list_init(&bios);
	cell_release(cell, &bios);

	while ((bio = bio_list_pop(&bios)))
		if (bio != exception)
			remap_and_issue(tc, bio, data_block);
}

static void retry_later(struct bio *bio)
{
	struct thin_c *tc = get_tc(bio);
	struct pool *pool = tc->pool;
	unsigned long flags;

	/* push it onto the retry list */
	spin_lock_irqsave(&pool->lock, flags);
	bio_list_add(&pool->retry_list, bio);
	spin_unlock_irqrestore(&pool->lock, flags);
}

static int alloc_data_block(struct thin_c *tc, dm_block_t *result)
{
	int r;
	dm_block_t free_blocks;
	unsigned long flags;
	struct pool *pool = tc->pool;

	r = dm_thin_metadata_get_free_blocks(pool->tmd, &free_blocks);
	if (r)
		return r;

	if (free_blocks <= pool->low_water_mark && !pool->triggered) {
		spin_lock_irqsave(&pool->lock, flags);
		pool->triggered = 1;
		spin_unlock_irqrestore(&pool->lock, flags);
		dm_table_event(pool->ti->table);
	}

	r = dm_thin_metadata_alloc_data_block(tc->td, result);
	if (r)
		return r;

	return 0;
}

static void process_discard(struct thin_c *tc, struct bio *bio)
{
	int r;
	dm_block_t block = get_bio_block(tc, bio);
	struct dm_thin_lookup_result lookup_result;

	r = dm_thin_metadata_lookup(tc->td, block, 1, &lookup_result);
	switch (r) {
	case 0:
		if (lookup_result.shared)
			/*
			 * We just ignore shared discards for now, these
			 * are hard, and I want to get deferred
			 * deallocation working first.
			 */
			bio_endio(bio, 0);

		else {
			r = dm_thin_metadata_remove(tc->td, block);
			if (r) {
				DMERR("dm_thin_metadata_remove() failed");
				bio_io_error(bio);
			} else
				remap_and_issue(tc, bio, lookup_result.block);
		}
		break;

	case -ENODATA:
		/* Either this isn't provisioned, or preparation for
		 * provisioning may be pending (we could find out by
		 * calling bio_detain_if_occupied).  But even in this case
		 * it's easier to just forget the discard.
		 */
		bio_endio(bio, 0);
		break;

	default:
		DMERR("dm_thin_metadata_lookup() failed, error = %d", r);
		bio_io_error(bio);
		break;
	}
}

static void no_space(struct cell *cell)
{
	struct bio *bio;
	struct bio_list bios;

	bio_list_init(&bios);
	cell_release(cell, &bios);
	while ((bio = bio_list_pop(&bios)))
		retry_later(bio);
}

static void break_sharing(struct thin_c *tc, struct bio *bio, dm_block_t block,
			  struct cell_key *key,
			  struct dm_thin_lookup_result *lookup_result)
{
	int r;
	dm_block_t data_block;
	struct cell *cell;

	bio_detain(tc->pool->prison, key, bio, &cell);

	r = alloc_data_block(tc, &data_block);
	switch (r) {
	case 0:
		schedule_copy(tc, block, lookup_result->block,
			      data_block, cell, bio);
		break;

	case -ENOSPC:
		no_space(cell);
		break;

	default:
		DMERR("%s: alloc_data_block() failed, error = %d", __func__, r);
		cell_error(cell);
		break;
	}
}

static void process_shared_bio(struct thin_c *tc, struct bio *bio,
			       dm_block_t block,
			       struct dm_thin_lookup_result *lookup_result)
{
	struct cell *cell;
	struct cell_key key;
	struct pool *pool = tc->pool;

	build_data_key(tc->td, lookup_result->block, &key);
	if (bio_detain_if_occupied(pool->prison, &key, bio, &cell))
		return; /* already underway */

	if (bio_data_dir(bio) == WRITE)
		break_sharing(tc, bio, block, &key, lookup_result);
	else {
		struct endio_hook *h = mempool_alloc(pool->endio_hook_pool,
						     GFP_NOIO);

		h->tc = tc;
		h->bi_end_io = bio->bi_end_io;
		h->bi_private = bio->bi_private;
		h->entry = ds_inc(&pool->ds);

		bio->bi_end_io = shared_read_complete;
		bio->bi_private = h;

		remap_and_issue(tc, bio, lookup_result->block);
	}
}

static void provision_block(struct thin_c *tc, struct bio *bio, dm_block_t block)
{
	int r;
	dm_block_t data_block;
	struct cell *cell;
	struct cell_key key;

	build_virtual_key(tc->td, block, &key);
	if (bio_detain(tc->pool->prison, &key, bio, &cell))
		return; /* already underway */

	r = alloc_data_block(tc, &data_block);
	switch (r) {
	case 0:
		schedule_zero(tc, block, data_block, cell, bio);
		break;

	case -ENOSPC:
		no_space(cell);
		break;

	default:
		DMERR("%s: alloc_data_block() failed, error = %d", __func__, r);
		cell_error(cell);
		break;
	}
}

static void process_bio(struct thin_c *tc, struct bio *bio)
{
	int r;
	dm_block_t block = get_bio_block(tc, bio);
	struct dm_thin_lookup_result lookup_result;

	r = dm_thin_metadata_lookup(tc->td, block, 1, &lookup_result);
	switch (r) {
	case 0:
		if (lookup_result.shared)
			process_shared_bio(tc, bio, block, &lookup_result);
		else
			remap_and_issue(tc, bio, lookup_result.block);
		break;

	case -ENODATA:
		if (bio_rw(bio) == READ || bio_rw(bio) == READA) {
			bio->bi_bdev = tc->pool_dev->bdev;
			zero_fill_bio(bio);
			bio_endio(bio, 0);
		} else
			provision_block(tc, bio, block);
		break;

	default:
		DMERR("dm_thin_metadata_lookup() failed, error = %d", r);
		bio_io_error(bio);
		break;
	}
}

static void process_bios(struct pool *pool)
{
	unsigned long flags;
	struct bio *bio;
	struct bio_list bios;

	bio_list_init(&bios);

	spin_lock_irqsave(&pool->lock, flags);
	bio_list_merge(&bios, &pool->deferred_bios);
	bio_list_init(&pool->deferred_bios);
	spin_unlock_irqrestore(&pool->lock, flags);

	while ((bio = bio_list_pop(&bios))) {
		struct thin_c *tc = get_tc(bio);

		if (bio->bi_rw & REQ_DISCARD)
			process_discard(tc, bio);
		else
			process_bio(tc, bio);
	}
}

static void process_prepared_mappings(struct pool *pool)
{
	int r;
	unsigned long flags;
	struct list_head maps;
	struct new_mapping *m, *tmp;
	struct bio *bio;

	INIT_LIST_HEAD(&maps);
	spin_lock_irqsave(&pool->lock, flags);
	list_splice_init(&pool->prepared_mappings, &maps);
	spin_unlock_irqrestore(&pool->lock, flags);

	list_for_each_entry_safe(m, tmp, &maps, list) {
		struct thin_c *tc = m->tc;

		if (m->err) {
			cell_error(m->cell);
			continue;
		}

		bio = m->bio;
		if (bio) {
			bio->bi_end_io = m->bi_end_io;
			bio->bi_private = m->bi_private;
		}

		r = dm_thin_metadata_insert(tc->td, m->virt_block,
					    m->data_block);
		if (r) {
			DMERR("dm_thin_metadata_insert() failed");
			cell_error(m->cell);
		} else {
			if (m->bio) {
				cell_remap_and_issue_except(tc, m->cell,
							    m->data_block, bio);
				bio_endio(bio, 0);
			} else
				cell_remap_and_issue(tc, m->cell, m->data_block);

			list_del(&m->list);
			mempool_free(m, pool->mapping_pool);
		}
	}
}

static void do_producer(struct work_struct *ws)
{
	struct pool *pool = container_of(ws, struct pool, producer);
	process_bios(pool);
}

static void do_consumer(struct work_struct *ws)
{
	struct pool *pool = container_of(ws, struct pool, consumer);
	process_prepared_mappings(pool);
}

static void defer_bio(struct thin_c *tc, struct bio *bio)
{
	unsigned long flags;
	struct pool *pool = tc->pool;

	set_tc(bio, tc);

	spin_lock_irqsave(&pool->lock, flags);
	bio_list_add(&pool->deferred_bios, bio);
	spin_unlock_irqrestore(&pool->lock, flags);

	wake_producer(pool);
}

/*
 * Non-blocking function designed to be called from the targets map
 * function.
 */
static int bio_map(struct dm_target *ti, struct bio *bio)
{
	int r;
	struct thin_c *tc = ti->private;
	dm_block_t block = get_bio_block(tc, bio);
	struct dm_thin_device *td = tc->td;
	struct pool *pool = tc->pool;
	struct dm_thin_lookup_result result;

	/*
	 * XXX(hch): in theory higher level code should prevent this
	 * from happening, not sure why we ever get here.
	 */
	if ((bio->bi_rw & REQ_DISCARD) &&
	    bio->bi_size < (pool->sectors_per_block << SECTOR_SHIFT)) {
		DMERR("discard IO smaller than pool block size (%llu)",
		      (unsigned long long)pool->sectors_per_block << SECTOR_SHIFT);
		bio_endio(bio, 0);
		return DM_MAPIO_SUBMITTED;
	}

	if (bio->bi_rw & (REQ_DISCARD | REQ_FLUSH | REQ_FUA)) {
		defer_bio(tc, bio);
		return DM_MAPIO_SUBMITTED;
	}

	r = dm_thin_metadata_lookup(td, block, 0, &result);
	switch (r) {
	case 0:
		if (unlikely(result.shared)) {
			/*
			 * We have a race condition here between the
			 * result.shared value returned by the lookup and
			 * snapshot creation, which may cause new
			 * sharing.
			 *
			 * To avoid this always quiesce the origin before
			 * taking the snap.  You want to do this anyway to
			 * ensure a consistent application view
			 * (i.e. lockfs).
			 *
			 * More distant ancestors are irrelevant, the
			 * shared flag will be set in their case.
			 */
			defer_bio(tc, bio);
			r = DM_MAPIO_SUBMITTED;
		} else {
			remap(tc, bio, result.block);
			r = DM_MAPIO_REMAPPED;
		}
		break;

	case -ENODATA:
		if (bio_rw(bio) == READA || bio_rw(bio) == READ) {
			zero_fill_bio(bio);
			bio_endio(bio, 0);
		} else
			defer_bio(tc, bio);
		r = DM_MAPIO_SUBMITTED;
		break;

	case -EWOULDBLOCK:
		defer_bio(tc, bio);
		r = DM_MAPIO_SUBMITTED;
		break;
	}

	return r;
}

static int pool_is_congested(struct dm_target_callbacks *cb, int bdi_bits)
{
	int r;
	unsigned long flags;
	struct pool_c *pt = container_of(cb, struct pool_c, callbacks);

	spin_lock_irqsave(&pt->pool->lock, flags);
	r = !bio_list_empty(&pt->pool->retry_list);
	spin_unlock_irqrestore(&pt->pool->lock, flags);

	if (!r) {
		struct request_queue *q = bdev_get_queue(pt->data_dev->bdev);
		r = bdi_congested(&q->backing_dev_info, bdi_bits);
	}

	return r;
}

static void __requeue_bios(struct pool *pool)
{
	struct bio *bio;
	struct bio_list bios;

	bio_list_init(&bios);
	/* retry_later() adds bios, that already have set_tc(), to retry_list */
	bio_list_merge(&bios, &pool->retry_list);
	bio_list_init(&pool->retry_list);

	/* FIXME: why wasn't this protected by &pool->lock like defer_bio() !? */
	while ((bio = bio_list_pop(&bios)))
		bio_list_add(&pool->deferred_bios, bio);
}

/*----------------------------------------------------------------
 * Binding of control targets to a pool object
 *--------------------------------------------------------------*/
/* FIXME: add locking */
static int bind_control_target(struct pool *pool, struct dm_target *ti)
{
	struct pool_c *pt = ti->private;

	pool->ti = ti;
	pool->low_water_mark = dm_div_up(pt->low_water_mark,
					 pool->sectors_per_block);
	pool->zero_new_blocks = pt->zero_new_blocks;
	dm_thin_metadata_rebind_block_device(pool->tmd, pt->metadata_dev->bdev);
	return 0;
}

static void unbind_control_target(struct pool *pool, struct dm_target *ti)
{
	if (pool->ti == ti)
		pool->ti = NULL;
}

/*----------------------------------------------------------------
 * Pool creation
 *--------------------------------------------------------------*/
static void pool_destroy(struct pool *pool)
{
	if (dm_thin_metadata_close(pool->tmd) < 0)
		DMWARN("%s: dm_thin_metadata_close() failed.", __func__);

	prison_destroy(pool->prison);
	dm_kcopyd_client_destroy(pool->copier);

	if (pool->producer_wq)
		destroy_workqueue(pool->producer_wq);

	if (pool->consumer_wq)
		destroy_workqueue(pool->consumer_wq);

	mempool_destroy(pool->mapping_pool);
	mempool_destroy(pool->endio_hook_pool);
	kfree(pool);
}

static struct pool *pool_create(struct block_device *metadata_dev,
				unsigned long block_size, char **error)
{
	int r;
	void *err_p;
	struct pool *pool;
	struct dm_thin_metadata *tmd;

	tmd = dm_thin_metadata_open(metadata_dev, block_size);
	if (IS_ERR(tmd)) {
		*error = "Error creating metadata object";
		err_p = tmd; /* already an ERR_PTR */
		return err_p;
	}

	pool = kmalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool) {
		*error = "Error allocating memory for pool";
		err_p = ERR_PTR(-ENOMEM);
		goto bad_pool;
	}

	pool->tmd = tmd;
	pool->sectors_per_block = block_size;
	pool->block_shift = ffs(block_size) - 1;
	pool->offset_mask = block_size - 1;
	pool->low_water_mark = 0;
	pool->zero_new_blocks = 1;
	pool->prison = prison_create(1024); /* FIXME: magic number */
	if (!pool->prison) {
		*error = "Error creating pool's bio prison";
		err_p = ERR_PTR(-ENOMEM);
		goto bad_prison;
	}

	pool->copier = dm_kcopyd_client_create();
	if (IS_ERR(pool->copier)) {
		r = PTR_ERR(pool->copier);
		*error = "Error creating pool's kcopyd client";
		err_p = ERR_PTR(r);
		goto bad_kcopyd_client;
	}

	/* Create singlethreaded workqueue that will service all devices
	 * that use this metadata.
	 */
	pool->producer_wq = alloc_ordered_workqueue(DM_MSG_PREFIX "-producer",
						    WQ_MEM_RECLAIM);
	if (!pool->producer_wq) {
		*error = "Error creating pool's producer workqueue";
		err_p = ERR_PTR(-ENOMEM);
		goto bad_producer_wq;
	}

	pool->consumer_wq = alloc_ordered_workqueue(DM_MSG_PREFIX "-consumer",
						    WQ_MEM_RECLAIM);
	if (!pool->consumer_wq) {
		*error = "Error creating pool's consumer workqueue";
		err_p = ERR_PTR(-ENOMEM);
		goto bad_consumer_wq;
	}

	INIT_WORK(&pool->producer, do_producer);
	INIT_WORK(&pool->consumer, do_consumer);
	spin_lock_init(&pool->lock);
	bio_list_init(&pool->deferred_bios);
	INIT_LIST_HEAD(&pool->prepared_mappings);
	pool->triggered = 0;
	bio_list_init(&pool->retry_list);
	ds_init(&pool->ds);
	/* FIXME: magic number */
	pool->mapping_pool =
		mempool_create_kmalloc_pool(1024, sizeof(struct new_mapping));
	if (!pool->mapping_pool) {
		*error = "Error creating pool's mapping mempool";
		err_p = ERR_PTR(-ENOMEM);
		goto bad_mapping_pool;
	}
	/* FIXME: magic number */
	pool->endio_hook_pool =
		mempool_create_kmalloc_pool(10240, sizeof(struct endio_hook));
	if (!pool->endio_hook_pool) {
		*error = "Error creating pool's endio_hook mempool";
		err_p = ERR_PTR(-ENOMEM);
		goto bad_endio_hook_pool;
	}
	atomic_set(&pool->ref_count, 1);

	return pool;

bad_endio_hook_pool:
	mempool_destroy(pool->mapping_pool);
bad_mapping_pool:
	destroy_workqueue(pool->consumer_wq);
bad_consumer_wq:
	destroy_workqueue(pool->producer_wq);
bad_producer_wq:
	dm_kcopyd_client_destroy(pool->copier);
bad_kcopyd_client:
	prison_destroy(pool->prison);
bad_prison:
	kfree(pool);
bad_pool:
	if (dm_thin_metadata_close(tmd))
		DMWARN("%s: dm_thin_metadata_close() failed.", __func__);

	return err_p;
}

static void pool_inc(struct pool *pool)
{
	atomic_inc(&pool->ref_count);
}

static void pool_dec(struct pool *pool)
{
	if (atomic_dec_and_test(&pool->ref_count))
		pool_destroy(pool);
}

static struct pool *pool_find(struct block_device *pool_bdev,
			      struct block_device *metadata_dev,
			      unsigned long block_size,
			      char **error)
{
	struct pool *pool;

	pool = pool_table_lookup(pool_bdev);
	if (pool)
		pool_inc(pool);
	else
		pool = pool_create(metadata_dev, block_size, error);

	return pool;
}

/*----------------------------------------------------------------
 * Pool target methods
 *--------------------------------------------------------------*/
static void pool_dtr(struct dm_target *ti)
{
	struct pool_c *pt = ti->private;

	dm_put_device(ti, pt->metadata_dev);
	dm_put_device(ti, pt->data_dev);
	unbind_control_target(pt->pool, ti);
	pool_dec(pt->pool);
	kfree(pt);
}

struct pool_features {
	unsigned zero_new_blocks;
};

static int parse_pool_features(struct dm_arg_set *as, struct pool_features *pf,
			       struct dm_target *ti)
{
	int r;
	unsigned argc;
	const char *arg_name;

	static struct dm_arg _args[] = {
		{0, 1, "Invalid number of pool feature arguments"},
	};

	/* No feature arguments supplied. */
	if (!as->argc)
		return 0;

	r = dm_read_arg_group(_args, as, &argc, &ti->error);
	if (r)
		return -EINVAL;

	while (argc && !r) {
		arg_name = dm_shift_arg(as);
		argc--;

		if (!strcasecmp(arg_name, "skip_block_zeroing")) {
			pf->zero_new_blocks = 0;
			continue;
		}

		ti->error = "Unrecognised pool feature requested";
		r = -EINVAL;
	}

	return r;
}

/*
 * thin-pool <metadata dev> <data dev>
 *           <data block size (sectors)>
 *           <low water mark (sectors)>
 *           [<#feature args> [<arg>]*]
 *
 * Optional feature arguments are:
 *           skip_block_zeroing: skips the zeroing of newly-provisioned blocks.
 */
static int pool_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	int r;
	struct pool_c *pt;
	struct pool *pool;
	struct pool_features pf;
	struct dm_arg_set as;
	struct dm_dev *data_dev;
	unsigned long block_size;
	dm_block_t low_water;
	struct dm_dev *metadata_dev;
	char *end;

	if (argc < 4) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}
	as.argc = argc;
	as.argv = argv;

	r = dm_get_device(ti, argv[0], FMODE_READ | FMODE_WRITE, &metadata_dev);
	if (r) {
		ti->error = "Error opening metadata block device";
		return r;
	}

	r = dm_get_device(ti, argv[1], FMODE_READ | FMODE_WRITE, &data_dev);
	if (r) {
		ti->error = "Error getting data device";
		goto out_data_dev;
	}

	block_size = simple_strtoul(argv[2], &end, 10);
	if (!block_size || *end) {
		ti->error = "Invalid block size";
		r = -EINVAL;
		goto out;
	}

	low_water = simple_strtoull(argv[3], &end, 10);
	if (!low_water || *end) {
		ti->error = "Invalid low water mark";
		r = -EINVAL;
		goto out;
	}

	/*
	 * Set default pool features.
	 */
	memset(&pf, 0, sizeof(pf));
	pf.zero_new_blocks = 1;

	dm_consume_args(&as, 4);
	r = parse_pool_features(&as, &pf, ti);
	if (r)
		goto out;

	pool = pool_find(get_target_bdev(ti), metadata_dev->bdev,
			 block_size, &ti->error);
	if (IS_ERR(pool)) {
		r = PTR_ERR(pool);
		goto out;
	}

	pt = kmalloc(sizeof(*pt), GFP_KERNEL);
	if (!pt) {
		pool_destroy(pool);
		r = -ENOMEM;
		goto out;
	}
	pt->pool = pool;
	pt->ti = ti;
	pt->metadata_dev = metadata_dev;
	pt->data_dev = data_dev;
	pt->low_water_mark = low_water;
	pt->zero_new_blocks = pf.zero_new_blocks;
	ti->num_flush_requests = 1;
	ti->num_discard_requests = 1;
	ti->private = pt;

	pt->callbacks.congested_fn = pool_is_congested;
	dm_table_add_target_callbacks(ti->table, &pt->callbacks);

	return 0;

out:
	dm_put_device(ti, data_dev);
out_data_dev:
	dm_put_device(ti, metadata_dev);

	return r;
}

static int pool_map(struct dm_target *ti, struct bio *bio,
		    union map_info *map_context)
{
	int r;
	struct pool_c *pt = ti->private;
	struct pool *pool = pt->pool;
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	bio->bi_bdev = pt->data_dev->bdev;
	r = DM_MAPIO_REMAPPED;
	spin_unlock_irqrestore(&pool->lock, flags);

	return r;
}

/*
 * Retrieves the number of blocks of the data device from
 * the superblock and compares it to the actual device size,
 * thus resizing the data device in case it has grown.
 *
 * This both copes with opening preallocated data devices in the ctr
 * being followed by a resume
 * -and-
 * calling the resume method individually after userspace has
 * grown the data device in reaction to a table event.
 */
static int pool_preresume(struct dm_target *ti)
{
	int r;
	struct pool_c *pt = ti->private;
	struct pool *pool = pt->pool;
	dm_block_t data_size, sb_data_size;
	unsigned long flags;

	/* take control of the pool object */
	r = bind_control_target(pool, ti);
	if (r)
		return r;

	data_size = ti->len >> pool->block_shift;
	r = dm_thin_metadata_get_data_dev_size(pool->tmd, &sb_data_size);
	if (r) {
		DMERR("failed to retrieve data device size");
		return r;
	}

	if (data_size < sb_data_size) {
		DMERR("pool target too small, is %llu blocks (expected %llu)",
		      data_size, sb_data_size);
		return -EINVAL;

	} else if (data_size > sb_data_size) {
		r = dm_thin_metadata_resize_data_dev(pool->tmd, data_size);
		if (r) {
			DMERR("failed to resize data device");
			return r;
		}

		r = dm_thin_metadata_commit(pool->tmd);
		if (r) {
			DMERR("%s: dm_thin_metadata_commit() failed, error = %d",
			      __func__, r);
			return r;
		}
	}

	spin_lock_irqsave(&pool->lock, flags);
	pool->triggered = 0;
	__requeue_bios(pool);
	spin_unlock_irqrestore(&pool->lock, flags);

	wake_producer(pool);

	/* The pool object is only present if the pool is active */
	pool->pool_dev = get_target_bdev(ti);
	pool_table_insert(pool);

	return 0;
}

static void pool_presuspend(struct dm_target *ti)
{
	int r;
	struct pool_c *pt = ti->private;
	struct pool *pool = pt->pool;

	r = dm_thin_metadata_commit(pool->tmd);
	if (r < 0) {
		DMERR("%s: dm_thin_metadata_commit() failed, error = %d",
		      __func__, r);
		/* FIXME: invalidate device? error the next FUA or FLUSH bio ?*/
	}
}

static void pool_postsuspend(struct dm_target *ti)
{
	struct pool_c *pt = ti->private;
	struct pool *pool = pt->pool;

	pool_table_remove(pool);
	pool->pool_dev = NULL;
}

/*
 * Messages supported:
 *   new-thin        <dev id>
 *   new-snap        <dev id> <origin id>
 *   del             <dev id>
 *   trim            <dev id> <size in sectors>
 *   trans-id        <current trans id> <new trans id>
 */
static int pool_message(struct dm_target *ti, unsigned argc, char **argv)
{
	/* ti->error doesn't have a const qualifier :( */
	char *invalid_args = "Incorrect number of arguments";

	int r;
	struct pool_c *pt = ti->private;
	struct pool *pool = pt->pool;
	dm_thin_dev_t dev_id;
	char *end;

	if (!strcmp(argv[0], "new-thin")) {
		if (argc != 2) {
			ti->error = invalid_args;
			return -EINVAL;
		}

		dev_id = simple_strtoull(argv[1], &end, 10);
		if (*end) {
			ti->error = "Invalid device id";
			return -EINVAL;
		}

		r = dm_thin_metadata_create_thin(pool->tmd, dev_id);
		if (r) {
			ti->error = "Creation of thin provisioned device failed";
			return r;
		}

	} else if (!strcmp(argv[0], "new-snap")) {
		dm_thin_dev_t origin_id;

		if (argc != 3) {
			ti->error = invalid_args;
			return -EINVAL;
		}

		dev_id = simple_strtoull(argv[1], &end, 10);
		if (*end) {
			ti->error = "Invalid device id";
			return -EINVAL;
		}

		origin_id = simple_strtoull(argv[2], &end, 10);
		if (*end) {
			ti->error = "Invalid origin id";
			return -EINVAL;
		}

		r = dm_thin_metadata_create_snap(pool->tmd, dev_id, origin_id);
		if (r) {
			ti->error = "Creation of snapshot failed";
			return r;
		}

	} else if (!strcmp(argv[0], "del")) {
		if (argc != 2) {
			ti->error = invalid_args;
			return -EINVAL;
		}

		dev_id = simple_strtoull(argv[1], &end, 10);
		if (*end) {
			ti->error = "Invalid device id";
			return -EINVAL;
		}

		r = dm_thin_metadata_delete_device(pool->tmd, dev_id);

	} else if (!strcmp(argv[0], "trim")) {
		sector_t new_size;

		if (argc != 3) {
			ti->error = invalid_args;
			return -EINVAL;
		}

		dev_id = simple_strtoull(argv[1], &end, 10);
		if (*end) {
			ti->error = "Invalid device id";
			return -EINVAL;
		}

		new_size = simple_strtoull(argv[2], &end, 10);
		if (*end) {
			ti->error = "Invalid size";
			return -EINVAL;
		}

		r = dm_thin_metadata_trim_thin_dev(
			pool->tmd, dev_id,
			dm_div_up(new_size, pool->sectors_per_block));
		if (r) {
			ti->error = "Couldn't trim thin device";
			return r;
		}

	} else if (!strcmp(argv[0], "trans-id")) {
		uint64_t old_id, new_id;

		if (argc != 3) {
			ti->error = invalid_args;
			return -EINVAL;
		}

		old_id = simple_strtoull(argv[1], &end, 10);
		if (*end) {
			ti->error = "Invalid current transaction id";
			return -EINVAL;
		}

		new_id = simple_strtoull(argv[2], &end, 10);
		if (*end) {
			ti->error = "Invalid new transaction id";
			return -EINVAL;
		}

		r = dm_thin_metadata_set_transaction_id(pool->tmd,
							old_id, new_id);
		if (r) {
			ti->error = "Setting userspace transaction id failed";
			return r;
		}
	} else
		return -EINVAL;

	if (!r) {
		r = dm_thin_metadata_commit(pool->tmd);
		if (r)
			DMERR("%s: dm_thin_metadata_commit() failed, error = %d",
			      __func__, r);
	}

	return r;
}

static int pool_status(struct dm_target *ti, status_type_t type,
		       char *result, unsigned maxlen)
{
	int r;
	unsigned sz = 0;
	uint64_t transaction_id;
	dm_block_t nr_free_blocks_data;
	dm_block_t nr_free_blocks_metadata;
	dm_block_t held_root;
	char buf[BDEVNAME_SIZE];
	char buf2[BDEVNAME_SIZE];
	struct pool_c *pt = ti->private;
	struct pool *pool = pt->pool;

	switch (type) {
	case STATUSTYPE_INFO:
		r = dm_thin_metadata_get_transaction_id(pool->tmd,
							&transaction_id);
		if (r)
			return r;

		r = dm_thin_metadata_get_free_blocks(pool->tmd,
						     &nr_free_blocks_data);
		if (r)
			return r;

		r = dm_thin_metadata_get_free_blocks_metadata(pool->tmd,
							      &nr_free_blocks_metadata);
		if (r)
			return r;

		r = dm_thin_metadata_get_held_root(pool->tmd, &held_root);
		if (r)
			return r;

		DMEMIT("%llu %llu %llu ", transaction_id,
		       nr_free_blocks_data * pool->sectors_per_block,
		       nr_free_blocks_metadata * pool->sectors_per_block);

		if (held_root)
			DMEMIT("%llu", held_root);
		else
			DMEMIT("-");

		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%s %s %lu %lu ",
		       format_dev_t(buf, pt->metadata_dev->bdev->bd_dev),
		       format_dev_t(buf2, pt->data_dev->bdev->bd_dev),
		       (unsigned long) pool->sectors_per_block,
		       (unsigned long) pt->low_water_mark);

		DMEMIT("%u ", !pool->zero_new_blocks);

		if (!pool->zero_new_blocks)
			DMEMIT("skip_block_zeroing ");
		break;
	}

	return 0;
}

static int pool_iterate_devices(struct dm_target *ti,
				iterate_devices_callout_fn fn, void *data)
{
	struct pool_c *pt = ti->private;
	return fn(ti, pt->data_dev, 0, ti->len, data);
}

static int pool_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
		      struct bio_vec *biovec, int max_size)
{
	struct pool_c *pt = ti->private;
	struct request_queue *q = bdev_get_queue(pt->data_dev->bdev);

	if (!q->merge_bvec_fn)
		return max_size;

	bvm->bi_bdev = pt->data_dev->bdev;
	return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

static void pool_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct pool_c *pt = ti->private;
	struct pool *pool = pt->pool;

	blk_limits_io_min(limits, 0);
	blk_limits_io_opt(limits, pool->sectors_per_block << SECTOR_SHIFT);
}

static struct target_type pool_target = {
	.name = "thin-pool",
	.features = DM_TARGET_SINGLETON,
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr = pool_ctr,
	.dtr = pool_dtr,
	.map = pool_map,
	.presuspend = pool_presuspend,
	.postsuspend = pool_postsuspend,
	.preresume = pool_preresume,
	.message = pool_message,
	.status = pool_status,
	.merge = pool_merge,
	.iterate_devices = pool_iterate_devices,
	.io_hints = pool_io_hints,
};

/*----------------------------------------------------------------*/

static void thin_dtr(struct dm_target *ti)
{
	struct thin_c *tc = ti->private;

	pool_dec(tc->pool);
	dm_thin_metadata_close_device(tc->td);
	dm_put_device(ti, tc->pool_dev);
	kfree(tc);
}

/*
 * Thin target parameters:
 *
 * <pool dev> <dev id>
 *
 * pool dev: the path to the pool (eg, /dev/mapper/my_pool)
 * dev id: the internal device identifier
 */
static int thin_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	int r;
	struct thin_c *tc;
	struct dm_dev *pool_dev;
	char *end;

	if (argc != 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	tc = ti->private = kzalloc(sizeof(*tc), GFP_KERNEL);
	if (!tc) {
		ti->error = "Out of memory";
		return -ENOMEM;
	}

	r = dm_get_device(ti, argv[0], FMODE_READ | FMODE_WRITE, &pool_dev);
	if (r) {
		ti->error = "Error opening pool device";
		goto bad_pool_dev;
	}
	tc->pool_dev = pool_dev;

	tc->dev_id = simple_strtoull(argv[1], &end, 10);
	if (*end) {
		ti->error = "Invalid device id";
		r = -EINVAL;
		goto bad_dev_id;
	}

	tc->pool = pool_table_lookup(tc->pool_dev->bdev);
	if (!tc->pool) {
		ti->error = "Couldn't find pool object";
		r = -EINVAL;
		goto bad_pool_lookup;
	}
	pool_inc(tc->pool);

	r = dm_thin_metadata_open_device(tc->pool->tmd, tc->dev_id, &tc->td);
	if (r) {
		ti->error = "Couldn't open thin internal device";
		goto bad_thin_open;
	}

	ti->split_io = tc->pool->sectors_per_block;
	ti->num_flush_requests = 1;
	ti->num_discard_requests = 1;
	/*
	 * allow discards to issued to the thin device even
	 * if the pool's data device doesn't support them.
	 */
	ti->discards_supported = 1;

	return 0;

bad_thin_open:
	pool_dec(tc->pool);
bad_pool_lookup:
bad_dev_id:
	dm_put_device(ti, tc->pool_dev);
bad_pool_dev:
	kfree(tc);

	return r;
}

static int thin_map(struct dm_target *ti, struct bio *bio,
		    union map_info *map_context)
{
	bio->bi_sector -= ti->begin;
	return bio_map(ti, bio);
}

static int thin_status(struct dm_target *ti, status_type_t type,
		       char *result, unsigned maxlen)
{
	int r;
	ssize_t sz = 0;
	dm_block_t mapped, highest;
	char buf[BDEVNAME_SIZE];
	struct thin_c *tc = ti->private;

	if (tc->td) {
		switch (type) {
		case STATUSTYPE_INFO:
			r = dm_thin_metadata_get_mapped_count(tc->td,
							      &mapped);
			if (r)
				return r;

			r = dm_thin_metadata_get_highest_mapped_block(tc->td,
								      &highest);
			if (r < 0)
				return r;

			DMEMIT("%llu ", mapped * tc->pool->sectors_per_block);
			if (r)
				DMEMIT("%llu", ((highest + 1) *
						tc->pool->sectors_per_block) - 1);
			else
				DMEMIT("-");
			break;

		case STATUSTYPE_TABLE:
			DMEMIT("%s %lu",
			       format_dev_t(buf, tc->pool_dev->bdev->bd_dev),
			       (unsigned long) tc->dev_id);
			break;
		}
	} else {
		DMEMIT("-");
	}

	return 0;
}

static int thin_bvec_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
			   struct bio_vec *biovec, int max_size)
{
	struct thin_c *tc = ti->private;
	struct pool *pool = tc->pool;

	/*
	 * We fib here, because the space may not have been provisioned yet
	 * we can't give a good answer.  It's better to return the block
	 * size, and incur extra splitting in a few cases than always
	 * return the smallest, page sized, chunk.
	 */
	return pool->sectors_per_block << SECTOR_SHIFT;
}

static int thin_iterate_devices(struct dm_target *ti,
				iterate_devices_callout_fn fn, void *data)
{
	struct thin_c *tc = ti->private;
	struct pool *pool;

	pool = pool_table_lookup(tc->pool_dev->bdev);
	if (!pool) {
		DMERR("%s: Couldn't find pool object", __func__);
		return -EINVAL;
	}

	return fn(ti, tc->pool_dev, 0, pool->sectors_per_block, data);
}

static void thin_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct thin_c *tc = ti->private;
	struct pool *pool;

	pool = pool_table_lookup(tc->pool_dev->bdev);
	if (!pool) {
		DMERR("%s: Couldn't find pool object", __func__);
		return;
	}

	blk_limits_io_min(limits, 0);
	blk_limits_io_opt(limits, pool->sectors_per_block << SECTOR_SHIFT);

	/*
	 * Only allow discard requests aligned to our block size, and make
	 * sure that we never get sent larger discard requests either.
	 */
	limits->max_discard_sectors = pool->sectors_per_block;
	limits->discard_granularity = pool->sectors_per_block << SECTOR_SHIFT;
}

static struct target_type thin_target = {
	.name = "thin",
	.version = {1, 0, 0},
	.module	= THIS_MODULE,
	.ctr = thin_ctr,
	.dtr = thin_dtr,
	.map = thin_map,
	.status = thin_status,
	.merge = thin_bvec_merge,
	.iterate_devices = thin_iterate_devices,
	.io_hints = thin_io_hints,
};

/*----------------------------------------------------------------*/

static int __init dm_thin_init(void)
{
	int r = dm_register_target(&thin_target);
	if (r)
		return r;

	r = dm_register_target(&pool_target);
	if (r)
		dm_unregister_target(&thin_target);

	pool_table_init();
	return r;
}

static void dm_thin_exit(void)
{
	dm_unregister_target(&thin_target);
	dm_unregister_target(&pool_target);
}

module_init(dm_thin_init);
module_exit(dm_thin_exit);

MODULE_DESCRIPTION(DM_NAME "device-mapper thin provisioning target");
MODULE_AUTHOR("Joe Thornber <thornber@redhat.com>");
MODULE_LICENSE("GPL");

/*----------------------------------------------------------------*/
