/*
 * Copyright (C) 2011 Red Hat GmbH. All rights reserved.
 *
 * This file is released under the GPL.
 *
 * Hierarchical Storage Management target.
 *
 * Features:
 * o manages a storage pool of blocks on a fast block device to
 *   allocate from in order to cache blocks of a slower cached device
 * o data block size selectable (2^^N)
 * o low water mark in hsm ctr line and status
 *   - status <chunks free> <chunks used>
 *   - userland to kernel message just be a single resume (no prior suspend)
 *   - status provide metadata stats, userland resizes via same
 *     mechanism as data extend
 *
 * FIXME:
 * o support DISCARD requests to free unused blocks
 * o support relocation of blocks to allow for hot spot removal
 *   and shrinking of the data device.
 * o eventually drop metadata store creation once userspace does it
 *
 */

static const char version[] = "1.0.59";

#include "dm.h"
#include "hsm-metadata.h"
#include "persistent-data/transaction-manager.h"

#include <asm/div64.h>

#include <linux/blkdev.h>
#include <linux/dm-io.h>
#include <linux/dm-kcopyd.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/slab.h>

/*----------------------------------------------------------------*/

#define	DM_MSG_PREFIX	"dm-hsm"
#define	DAEMON		DM_MSG_PREFIX	"d"

/* Minimum data device block size in sectors. */
#define	DATA_DEV_BLOCK_SIZE_MIN	8
#define	LLU	long long unsigned

#define	MIN_IOS	32
#define	PARALLEL_COPIES	16

/* Cache for block io housekeeping. */
static struct kmem_cache *block_cache;

enum hsm_c_flags {
	HC_BOUNCE_MODE,
	HC_ERROR_EVENT,
	HC_NO_SPACE,
	HC_REFLUSHED,
};

/* Hierarchical storage context. */
struct hsm_c {
	struct dm_target *ti;
	struct hsm_metadata *hmd;
	hsm_dev_t dev;

	struct dm_dev *cached_dev;
	struct dm_dev *data_dev;
	struct dm_dev *meta_dev;

	struct list_head hsm_blocks;
	struct list_head flush_blocks;
	struct list_head endio_blocks;
	spinlock_t endio_lock;

	mempool_t *block_pool;

	sector_t block_sectors;
	sector_t offset_mask;	/* mask for offset of a sector within a block */
	unsigned int block_shift; /* Quick sector -> block mapping. */

	spinlock_t lock;	/* Protects central input list below. */
	struct bio_list in;	/* Bio input queue. */
	struct dm_kcopyd_client *kcopyd_client; /* kcopyd data <-> cached dev.*/

	spinlock_t no_space_lock;
	struct bio_list no_space; /* Bios w/o metadata space. */

	struct delayed_work dws;  /* IO work. */

	spinlock_t provisioned_lock; /* protects next 4 fields */
	sector_t data_sectors;	/* Size of data device in sectors. */
	block_t data_blocks;	/* Size of data device in blocks. */
	sector_t cached_sectors;/* Size of cached device in sectors. */
	block_t provisioned_count;
	block_t updates_since_last_commit;
	atomic_t dirty_blocks;
	int allocations;
	unsigned long flags;
	atomic_t block_writes;
	wait_queue_head_t pending_block_io;
};

struct hsm_block {
	atomic_t ref;
	struct hsm_c *hc;
	struct list_head list, flush_endio;
	struct bio_list io;
	struct bio_list endio;
	spinlock_t endio_lock;
	block_t cache_block, pool_block;
	unsigned long flags, timeout;
	void *bio_destructor;
};

enum block_flags {
	BLOCK_UPTODATE,
	BLOCK_DIRTY,
	/* Only max 4 persistent flags valid with hsm-metadata.c! */

	/* Non-persistent flags start here */
	BLOCK_ACTIVE = 4,
	BLOCK_ERROR,
	BLOCK_FORCE_DIRTY,
};

/* REMOVEME: for test lookup and display. */
static void _try_lookup(struct hsm_c *hc, block_t cache_block)
{
	int r;
	block_t cache_block1, pool_block;
	unsigned long flags;

	r = hsm_metadata_lookup(hc->hmd, hc->dev, cache_block, 1,
				    &pool_block, &flags);
	if (!r)
		DMINFO("%s 1. cache_block=%llu pool_block=%llu flags=%lu",
			__func__, (LLU) cache_block, (LLU) pool_block, flags);

	r = hsm_metadata_lookup_reverse(hc->hmd, hc->dev, pool_block, 1,
					&cache_block1);
	if (!r)
		DMINFO("%s 2. cache_block=%llu pool_block=%llu flags=%lu",
			__func__, (LLU) cache_block1, (LLU) pool_block, flags);
}

/*
 * Create/get a reference on an active block io housekeeping object.
 */
static void _get_block(struct hsm_block *b)
{
	atomic_inc(&b->ref);
}

static struct hsm_block *get_block(struct hsm_c *hc, block_t cache_block)
{
	struct hsm_block *b;

	/* FIXME: hash this! */
	list_for_each_entry(b, &hc->hsm_blocks, list) {
		if (b->cache_block == cache_block) {
			_get_block(b);
			goto out;
		}
	}

	b = mempool_alloc(hc->block_pool, GFP_NOIO);
	if (b) {
		memset(b, 0, sizeof(*b));
		atomic_set(&b->ref, 1);
		b->hc = hc;
		INIT_LIST_HEAD(&b->flush_endio);
		bio_list_init(&b->io);
		bio_list_init(&b->endio);
		spin_lock_init(&b->endio_lock);
		b->cache_block = cache_block;
		list_add(&b->list, &hc->hsm_blocks);
	}
out:
	return b;
}

static void put_block(struct hsm_block *b)
{
	if (atomic_dec_and_test(&b->ref)) {
		struct hsm_c *hc = b->hc;

		list_del(&b->list);
		mempool_free(b, hc->block_pool);

		if (list_empty(&hc->hsm_blocks))
			wake_up_all(&hc->pending_block_io);
	} else
		BUG_ON(atomic_read(&b->ref) < 0);
}

/* Return size of device in sectors. */
static sector_t get_dev_size(struct dm_dev *dev)
{
	return i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT;
}

/* Convert sector to block. */
static block_t _sector_to_block(struct hsm_c *hc, sector_t sector)
{
	return sector >> hc->block_shift;
}

/* Convert block to sector. */
static sector_t _block_to_sector(struct hsm_c *hc, block_t block)
{
	return block << hc->block_shift;
}

static void wake_do_hsm_delayed(struct hsm_c *hc, unsigned long delay)
{
	if (delayed_work_pending(&hc->dws)) {
		if (delay) {
			unsigned long j = jiffies,
				      expires = hc->dws.timer.expires > j ?
						hc->dws.timer.expires - j : 0;

			if (expires && expires < delay)
				return;
		}

		cancel_delayed_work(&hc->dws);
	}

	queue_delayed_work(hsm_metadata_get_workqueue(hc->hmd),
			   &hc->dws, delay);
}

static void wake_do_hsm(struct hsm_c *hc)
{
	wake_do_hsm_delayed(hc, 0);
}

static struct block_device *_remap_dev(struct hsm_c *hc)
{
       return hc->data_dev->bdev;
}

static sector_t _remap_sector(struct hsm_c *hc, sector_t sector, block_t block)
{
	return _block_to_sector(hc, block) + (sector & hc->offset_mask);
}

static void remap_bio(struct hsm_c *hc, struct bio *bio, block_t block)
{
	bio->bi_sector = _remap_sector(hc, bio->bi_sector, block);
	bio->bi_bdev = _remap_dev(hc);

	BUG_ON(bio->bi_sector >= hc->data_sectors);
}

/* Block copy callback (dm-kcopyd). */
static void block_copy_endio(int read_err, unsigned long write_err,
                             void *context)
{
	struct hsm_block *b = context;

	if (read_err || write_err)
		set_bit(BLOCK_ERROR, &b->flags);

	spin_lock(&b->hc->endio_lock);
	list_add(&b->flush_endio, &b->hc->endio_blocks);
	spin_unlock(&b->hc->endio_lock);

	wake_do_hsm(b->hc);
}

/* Copy blocks between cache and original (cached) device. */
static int block_copy(int rw, struct hsm_block *b)
{
	int r = 0;

	if (!test_and_set_bit(BLOCK_ACTIVE, &b->flags)) {
		struct hsm_c *hc = b->hc;
		struct dm_io_region cache = {
			.bdev = hc->data_dev->bdev,
			.sector = _remap_sector(hc, 0, b->pool_block),
		}, orig = {
			.bdev = hc->cached_dev->bdev,
			.sector = _remap_sector(hc, 0, b->cache_block),
		}, *from, *to;

		BUG_ON(cache.sector >= hc->data_sectors);

		/* Check for partial extent at origin device end. */
		cache.count = orig.count = min(hc->block_sectors,
					       hc->data_sectors - orig.sector);

		/* Set source and destination. */
		if (rw == READ) {
			from = &orig,  to = &cache;
		} else {
			atomic_inc(&hc->block_writes);
			from = &cache, to = &orig;
		}

		r = dm_kcopyd_copy(hc->kcopyd_client, from, 1, to, 0,
				   block_copy_endio, b);
	}

	return r;
}

static void _requeue_bios(struct bio_list *bl)
{
	struct bio *bio;

	while ((bio = bio_list_pop(bl)))
		bio_endio(bio, DM_ENDIO_REQUEUE);
}

static void requeue_bios(struct hsm_c *hc, struct bio_list *bl,
			 spinlock_t *lock)
{
	struct bio_list bios;

	bio_list_init(&bios);

	spin_lock(lock);
	bio_list_merge(&bios, bl);
	bio_list_init(bl);
	spin_unlock(lock);

	_requeue_bios(&bios);
}

static void requeue_all_bios(struct hsm_c *hc)
{
	requeue_bios(hc, &hc->in, &hc->lock);
	requeue_bios(hc, &hc->no_space, &hc->no_space_lock);
}

static int _congested(struct dm_dev *dev, int bdi_bits)
{
	struct request_queue *q = bdev_get_queue(dev->bdev);

	return bdi_congested(&q->backing_dev_info, bdi_bits);
}

/* Hierachical storage congested function. */
static int hc_congested(void *congested_data, int bdi_bits)
{
	struct hsm_c *hc = congested_data;

	return test_bit(HC_NO_SPACE, &hc->flags) ||
	       _congested(hc->cached_dev, bdi_bits) ||
	       _congested(hc->data_dev,   bdi_bits) ||
	       _congested(hc->meta_dev,   bdi_bits);
}

/* Set congested function. */
static void hc_set_congested_fn(struct hsm_c *hc)
{
	struct mapped_device *md = dm_table_get_md(hc->ti->table);
	struct backing_dev_info *bdi = &dm_disk(md)->queue->backing_dev_info;

	/* Set congested function and data. */
	bdi->congested_fn = hc_congested;
	bdi->congested_data = hc;
}

/* @allocated: amount to add to blocks allocated. */
static void inc_update(struct hsm_c *hc, int allocated)
{
	spin_lock(&hc->provisioned_lock);
	hc->updates_since_last_commit++;
	hc->allocations += allocated;
	spin_unlock(&hc->provisioned_lock);
}

/* If commit fails, log an error and throw an event. */
static int commit(struct hsm_c *hc)
{
	int allocations, r = 0;
	block_t updates;

	spin_lock(&hc->provisioned_lock);
	updates = hc->updates_since_last_commit;
	allocations = hc->allocations;
	hc->updates_since_last_commit = 0;
	hc->allocations = 0;
	spin_unlock(&hc->provisioned_lock);

	if (updates) {
		r = hsm_metadata_commit(hc->hmd);
		if (r) {
			/*
			 * FIXME: invalidate device?
			 * error the next FUA or FLUSH bio ?
			 */
			if (!test_and_set_bit(HC_ERROR_EVENT, &hc->flags)) {
				DMERR_LIMIT("hsm metadata write failed.");
				dm_table_event(hc->ti->table);
			}
		} else {
			spin_lock(&hc->provisioned_lock);
			hc->provisioned_count += allocations;
			spin_unlock(&hc->provisioned_lock);
		}
	}

	return r;
}

static void do_schedule_block_flush(struct hsm_c *hc, unsigned long j)
{
	if (!list_empty(&hc->flush_blocks)) {
		unsigned long timeout =
			list_first_entry(&hc->flush_blocks, struct hsm_block,
					 flush_endio)->timeout;
		wake_do_hsm_delayed(hc, j < timeout ? timeout - j : 0);
	}
}

/* Insert into flash list sorted by timeout. */
static void flush_add_sorted(struct hsm_block *b)
{
	BUG_ON(!test_bit(BLOCK_DIRTY, &b->flags));

	if (!test_bit(BLOCK_ACTIVE, &b->flags) &&
	    list_empty(&b->flush_endio)) {
		_get_block(b);
		b->timeout = jiffies + 3 * HZ;
		list_add_tail(&b->flush_endio, &b->hc->flush_blocks);
		atomic_inc(&b->hc->dirty_blocks);
// DMINFO("Added cache_block=%llu pool_block=%llu to flush", (LLU) b->cache_block, (LLU) b->pool_block);
	}
}

/* Clear out flush list on suspend. */
static void clear_flush_blocks(struct hsm_c *hc)
{
	struct hsm_block *b, *tmp;

	list_for_each_entry_safe(b, tmp, &hc->flush_blocks, flush_endio) {
		BUG_ON(atomic_read(&b->ref) != 1);
		put_block(b);
	}
}

/* Process any write updating metadata and delaying respective block flush. */
static int _process_write(struct hsm_block *b, struct bio *bio)
{
	int r = 0;

	if (bio_data_dir(bio) == WRITE) {
		if (test_and_set_bit(BLOCK_DIRTY, &b->flags)) {
			if (test_bit(BLOCK_ACTIVE, &b->flags) ||
			    b->timeout != ~0)
				set_bit(BLOCK_FORCE_DIRTY, &b->flags);

		} else {
			int r = hsm_metadata_update(b->hc->hmd, b->hc->dev,
						    b->cache_block, b->flags);
			if (r) /* Fatal. */
				bio_io_error(bio);

			else {
				flush_add_sorted(b);
				inc_update(b->hc, 0);
			}
// _try_lookup(b->hc, b->cache_block);
		}
	}

	return r;
}

/* Remap and submit a bio. */
static void _generic_make_request(struct hsm_block *b, struct bio *bio)
{
	remap_bio(b->hc, bio, b->pool_block);
	generic_make_request(bio);
}

/* Put any dirty blocks onto flush queue after a restart. */
static void do_leftover_dirty_blocks(struct hsm_c *hc)
{
	int r;
	unsigned long flags;
	block_t cache_block, pool_block, pool_block1;
	struct hsm_block *b;

	for (pool_block = 0; pool_block < hc->data_blocks; pool_block++) {
		/* Reverse lookup cache block by pool block. */
		r = hsm_metadata_lookup_reverse(hc->hmd, hc->dev, pool_block,
						1, &cache_block);
		if (r)
			continue;

		/* Retrieve persistent flags. */
		r = hsm_metadata_lookup(hc->hmd, hc->dev, cache_block, 1,
					&pool_block1, &flags);
		if (r || !test_bit(BLOCK_DIRTY, &flags))
			continue;

		BUG_ON(pool_block != pool_block1);

		/* Schedule any dirty blocks for io. */
		b = get_block(hc, cache_block);
		BUG_ON(!b);
		b->pool_block = pool_block;
		b->flags = flags;
DMINFO("Adding pool_block=%llu flags=%lu to flush list", (LLU) pool_block, b->flags);
		flush_add_sorted(b);
		put_block(b);
	}
}

/* Process all endios on blocks. */
static void do_endios(struct hsm_c *hc)
{
	int meta_err, r = 0;
	struct hsm_block *b, *tmp;
	LIST_HEAD(endios);

	spin_lock_irq(&hc->endio_lock);
	list_splice(&hc->endio_blocks, &endios);
	INIT_LIST_HEAD(&hc->endio_blocks);
	spin_unlock_irq(&hc->endio_lock);

	/*
	 * First round to check, if any metadata updates are
	 * mandatory and need to hit the metadata device
	 * _before_ any dependent io may be submitted.
	 */
	list_for_each_entry(b, &endios, flush_endio) {
		BUG_ON(!test_and_clear_bit(BLOCK_ACTIVE, &b->flags));

		if (test_bit(BLOCK_UPTODATE, &b->flags))
			atomic_dec(&hc->block_writes);

		if (!test_bit(BLOCK_ERROR, &b->flags)) {
			int update = 0;

			if (test_and_set_bit(BLOCK_UPTODATE, &b->flags)) {
				atomic_dec(&hc->dirty_blocks);

				/*
				 * Reforce write;
				 * we've been written to again.
				 */
				if (!test_and_clear_bit(BLOCK_FORCE_DIRTY,
							&b->flags)) {
					clear_bit(BLOCK_DIRTY, &b->flags);
					update = 1;
				}
			} else
				update = 1;

			if (update) {
				r = hsm_metadata_update(hc->hmd, hc->dev,
							b->cache_block,
							b->flags);
				if (r)
					meta_err = r;

				else
					inc_update(hc, 0);
			}
		}
	}

	/* Now commit any metadata transaction. */
	if (!meta_err)
		meta_err = commit(hc);

	/* Second round to submit the actual io _after_ any metadata commit. */
	list_for_each_entry_safe(b, tmp, &endios, flush_endio) {
		int err;
		struct bio *bio;
		struct bio_list bios;

		bio_list_init(&bios);
		list_del_init(&b->flush_endio);

		if (test_bit(BLOCK_DIRTY, &b->flags))
			flush_add_sorted(b);

		spin_lock_irq(&b->endio_lock);
		bio_list_merge(&bios, &b->endio);
		bio_list_init(&b->endio);
		spin_unlock_irq(&b->endio_lock);

		/* Error all pending io if we got a commit error. */
		err = (meta_err < 0 ||
		       test_bit(BLOCK_ERROR, &b->flags)) ? -EIO : 0;

		while ((bio = bio_list_pop(&bios)))
			bio_endio(bio, err);

		/* Submit any bios waiting for io on this block. */
		while ((bio = bio_list_pop(&b->io))) {
			if (err)
				bio_endio(bio, err);

			else {
// DMINFO("%s %s sector=%llu", __func__, bio_data_dir(bio) == WRITE ? "writing" : "reading", (LLU) bio->bi_sector);
				r = _process_write(b, bio);
				if (!r)
					_generic_make_request(b, bio);
			}
		}

		put_block(b); /* Release reference for block_copy(); */
	}

	commit(hc); /* FIXME: error handling. */
}

/* Process all bios. */
static void do_bios(struct hsm_c *hc, struct bio_list *bios)
{
	int meta_err = 0, r;
	struct bio *bio;
	struct hsm_block *b;

	/* 1/3: process all bios attaching to block objects. */
	while ((bio = bio_list_pop(bios))) {
		unsigned long flags;
		block_t cache_block, pool_block;

		cache_block = _sector_to_block(hc, bio->bi_sector);
		r = hsm_metadata_lookup(hc->hmd, hc->dev, cache_block, 1,
					&pool_block, &flags);
		if (r == -ENODATA) {
			/* New mapping */
			r = hsm_metadata_insert(hc->hmd, hc->dev, cache_block,
					        &pool_block, &flags);
			if (!r)
				inc_update(hc, 1);

			else if (r == -ENOSPC) {
				/*
				 * No data space, so we postpone the bio
				 * until we evict blocks from the cache or
				 * more space added by userland.
				 */
				spin_lock(&hc->no_space_lock);
				bio_list_add(&hc->no_space, bio);
				set_bit(HC_NO_SPACE, &hc->flags);
				spin_unlock(&hc->no_space_lock);

				continue;
			}
		}

		/* Get the block housekeeping object for this bio. */
		b = get_block(hc, cache_block);
		BUG_ON(!b);

		/*
		 * Set the rest of the block object members only on creation.
		 * This can only be true, when the block has been inactive.
		 */
		if (atomic_read(&b->ref) == 1) {
			b->pool_block = pool_block;
			b->flags = flags;
		}

		/*
		 * Squirrel the active block io object
		 * reference to the end io function.
		 * bio->bi_destructor got preserved in the map function
		 * and will be restored at the end of the end_io function.
		 *
		 * Submit later, because all bio endio
		 * processing may occur after this only
		 * so that the end_io method is able to
		 * reference the block!
		 */
		bio->bi_destructor = (void*) b;

		if (r)
			goto bio_error;

		/*
		 * REQ_FUA should only trigger a commit() if it's
		 * to a block that is pending.  I'm not sure
		 * whether the overhead of tracking pending blocks
		 * is worth it though.
		 */
		if ((bio->bi_rw & (REQ_FUA | REQ_FLUSH))) {
			r = commit(hc);
			if (!meta_err)
				meta_err = r;

			if (r < 0) {
bio_error:
				bio_io_error(bio);
				continue;
			}
		}

		bio_list_add(&b->io, bio);
	}

	/* 2/3: check for completely written over blocks. */
	/* Set block uptodate if completely written over or read it. */
	list_for_each_entry(b, &hc->hsm_blocks, list) {
		sector_t sectors = 0;

		if (test_bit(BLOCK_UPTODATE, &b->flags) ||
		    test_bit(BLOCK_ACTIVE, &b->flags))
			continue;

		bio_list_for_each(bio, &b->io) {
			if (bio_data_dir(bio) == WRITE)
				sectors += bio_sectors(bio);
		}

		if (sectors == hc->block_sectors) {
			set_bit(BLOCK_DIRTY, &b->flags);
			set_bit(BLOCK_UPTODATE, &b->flags);
			r = hsm_metadata_update(hc->hmd, hc->dev,
						b->cache_block, b->flags);
			if (r)
				meta_err = r;

			else {
				flush_add_sorted(b);
				inc_update(hc, 0);
			}
		} else {
			_get_block(b);
			BUG_ON(block_copy(READ, b));
		}
	}

	if (!meta_err)
		meta_err = commit(hc);

	/* 3/3: submit bios. */
	list_for_each_entry(b, &hc->hsm_blocks, list) {
		if (test_bit(BLOCK_UPTODATE, &b->flags) || meta_err) {
			while ((bio = bio_list_pop(&b->io))) {
				if (meta_err)
					bio_io_error(bio);

				else {
					r = _process_write(b, bio);
					if (r) {
						meta_err = r;
						bio_io_error(bio);
					} else
						_generic_make_request(b, bio);
				}
			}
		}
	}

	commit(hc); /* FIXME: error handling. */
}

/* Process any delayed block writes. */
static void do_block_flushs(struct hsm_c *hc)
{
	int copies = 0;
	struct hsm_block *b, *tmp;
	unsigned long j = jiffies;

	list_for_each_entry_safe(b, tmp, &hc->flush_blocks, flush_endio) {
		if (atomic_read(&hc->block_writes) > PARALLEL_COPIES / 2)
			break;

		if (j >= b->timeout) {
			if (!test_bit(BLOCK_ACTIVE, &b->flags)) {
				list_del_init(&b->flush_endio);
				b->timeout = ~0;
// DMINFO("Writing block logical=%llu physical=%llu", (LLU) b->cache_block, b->pool_block);
				BUG_ON(block_copy(WRITE, b));
				copies++;
			}
		} else
			break; /* Bail out, flush list is sorted by timeout. */
	}

	if (0) { // copies) {
		/* Unplug devices. */
		blk_unplug(bdev_get_queue(hc->cached_dev->bdev));
		blk_unplug(bdev_get_queue(hc->data_dev->bdev));
	}
}

/* Check for block inactive, ie. no copy io or bios pending. */
static int block_inactive(struct hsm_c *hc,
			  block_t pool_block, block_t *cache_block)
{
	/* Reverse lookup cache block by pool block. */
	int r = hsm_metadata_lookup_reverse(hc->hmd, hc->dev, pool_block,
					    0, cache_block);
	if (!r) {
		struct hsm_block *b = get_block(hc, *cache_block);
		BUG_ON(!b);
		r = (atomic_read(&b->ref) == 1);
		put_block(b);
	}

	return r;
}

/*
 * Free an allocated block.
 *
 * We presume that the cache is fully allocated,
 * thus we can free an idle block randomly.
 *
 * FIXME: may end up finding no inactive block even if
 * 	  there's some _because_ of its random pattern.
 */
static void do_block_free(struct hsm_c *hc)
{
	if (test_bit(HC_NO_SPACE, &hc->flags)) {
		unsigned rand = random32();
		block_t blocks, cache, pool,
			start = do_div(rand, hc->data_blocks);

		for (blocks = 0, pool = start;
		     blocks < hc->data_blocks;
		     blocks++, pool++) {
			int r = block_inactive(hc, pool, &cache);
			if (r > 0) {
				struct bio_list bios;

				bio_list_init(&bios);
DMINFO("Freeing pool_block=%llu", (LLU) pool);
				r = hsm_metadata_remove(hc->hmd, hc->dev, cache);
				if (!r) {
					inc_update(hc, -1);
					commit(hc);
				}

				spin_lock(&hc->no_space_lock);
				bio_list_merge(&bios, &hc->no_space);
				bio_list_init(&hc->no_space);
				spin_unlock(&hc->no_space_lock);

				spin_lock(&hc->lock);
				bio_list_merge(&hc->in, &bios);
				clear_bit(HC_NO_SPACE, &hc->flags);
				spin_unlock(&hc->lock);

				wake_do_hsm(hc);
				break;
			} else if (pool == hc->data_blocks)
				pool = -1;
		}
	}
}

/* Main worker function. */
static void do_hsm(struct work_struct *ws)
{
	int r;
	struct hsm_c *hc = container_of(ws, struct hsm_c, dws.work);
	int bounce_mode = test_bit(HC_BOUNCE_MODE, &hc->flags);
	struct bio_list bios;

	bio_list_init(&bios);

	/* FIXME: do leftover dirty blocks in chunks to reduce allocation. */
	if (!bounce_mode &&
	    !test_and_set_bit(HC_REFLUSHED, &hc->flags))
		do_leftover_dirty_blocks(hc);

	do_endios(hc);

	spin_lock(&hc->lock);
	bio_list_merge(&bios, &hc->in);
	bio_list_init(&hc->in);
	spin_unlock(&hc->lock);

	if (bounce_mode)
		_requeue_bios(&bios);
	else
		do_bios(hc, &bios);

	if (!bounce_mode) {
		do_block_flushs(hc);
		do_block_free(hc);
		do_schedule_block_flush(hc, jiffies);
	}
}

static void hsm_flush(struct dm_target *ti)
{
	struct hsm_c *hc = ti->private;

	/* Wait until all io has been processed. */
	flush_workqueue(hsm_metadata_get_workqueue(hc->hmd));
	commit(hc);	/* FIXME: error handling. */
}

/* Destroy a hsm device mapping. */
static void hsm_dtr(struct dm_target *ti)
{
	struct hsm_c *hc = ti->private;

	/* Destroy hsm block pool. */
	if (hc->block_pool)
		mempool_destroy(hc->block_pool);

	/* Destroy kcopyd client. */
	if (hc->kcopyd_client)
		dm_kcopyd_client_destroy(hc->kcopyd_client);

	/* Close hsm metadata handler. */
	if (hc->hmd)
		hsm_metadata_close(hc->hmd);

	/* Release reference on cached device. */
	if (hc->cached_dev)
		dm_put_device(ti, hc->cached_dev);

	/* Release reference on data device. */
	if (hc->data_dev)
		dm_put_device(ti, hc->data_dev);

	/* Release reference on metadata device. */
	if (hc->meta_dev)
		dm_put_device(ti, hc->meta_dev);

	kfree(hc);
}

/* Parse constructor arguments. */
static int _parse_args(struct dm_target *ti, unsigned argc, char **argv,
		       sector_t *block_sectors)
{
	unsigned long long tmp;

	if (argc != 4) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	if (sscanf(argv[3], "%llu", &tmp) != 1 ||
	    tmp < DATA_DEV_BLOCK_SIZE_MIN ||
	    !is_power_of_2(tmp)) {
		ti->error = "Invalid data block size argument";
		return -EINVAL;
	}

	*block_sectors = tmp;
	return 0;
}

static int __get_device(struct dm_target *ti, char *arg, struct dm_dev **dev,
			char *errstr)
{
	int r = dm_get_device(ti, arg, FMODE_READ | FMODE_WRITE, dev);

	if (r)
		ti->error = errstr;

	return r;
}

static int _get_devices(struct hsm_c *hc, char **argv)
{
	return __get_device(hc->ti, argv[0], &hc->cached_dev,
			    "Error opening cached device") ||
	       __get_device(hc->ti, argv[1], &hc->data_dev,
			    "Error opening data device") ||
	       __get_device(hc->ti, argv[2], &hc->meta_dev,
			    "Error opening metadata device");

}

static int create_hsd(struct hsm_c *hc)
{
	hc->hmd = hsm_metadata_open(hc->meta_dev->bdev, hc->block_sectors,
				    hc->data_blocks);
	if (hc->hmd)
		DMINFO("%s hsm metadata dev opened", __func__);
	else {
		DMERR("%s couldn't open hsm metadata object", __func__);
		return -ENOMEM;
	}

	/* Get already provisioned blocks. */
	return hsm_metadata_get_provisioned_blocks(hc->hmd, hc->dev,
						   &hc->provisioned_count);
}

/*
 * Construct a hierarchical storage device mapping:
 *
 * <start> <length> hsm <cached_dev> <data_dev> <meta_dev>  <data_block_size>
 * cached_dev: slow cached device holding original data blocks;
 * 	       can be any preexisting slow device to be cached
 * data_dev: fast device holding cached data blocks
 * meta_dev: fast device keeping track of provisioned cached blocks
 * data_block_size: cache unit size in sectors
 *
 */
static int hsm_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	int r;
	sector_t block_sectors = 0;
	struct hsm_c *hc;

	r = _parse_args(ti, argc, argv, &block_sectors);
	if (r)
		return r;

	hc = ti->private = kzalloc(sizeof(*hc), GFP_KERNEL);
	if (!hc) {
		ti->error = "Error allocating hsm context";
		return -ENOMEM;
	}

	hc->ti = ti;
	hc->dev = 1; /* Fixed dev number for the time being. */
	INIT_LIST_HEAD(&hc->hsm_blocks);
	INIT_LIST_HEAD(&hc->flush_blocks);
	INIT_LIST_HEAD(&hc->endio_blocks);
	hc->block_sectors = block_sectors;
	hc->block_shift = ffs(block_sectors) - 1;
	hc_set_congested_fn(hc);

	spin_lock_init(&hc->lock);
	spin_lock_init(&hc->endio_lock);
	bio_list_init(&hc->in);
	spin_lock_init(&hc->no_space_lock);
	bio_list_init(&hc->no_space);
	atomic_set(&hc->block_writes, 0);
	atomic_set(&hc->dirty_blocks, 0);
	init_waitqueue_head(&hc->pending_block_io);

	r = _get_devices(hc, argv);
	if (r)
		goto err;

	r = dm_kcopyd_client_create((block_sectors >> (PAGE_SHIFT - SECTOR_SHIFT)) * PARALLEL_COPIES, &hc->kcopyd_client);
	if (r) 
		goto err;

	hc->block_pool = mempool_create_slab_pool(MIN_IOS, block_cache);
        if (!hc->block_pool)
		goto err;

	hc->data_sectors = get_dev_size(hc->data_dev);
	hc->data_blocks = _sector_to_block(hc, hc->data_sectors);
	hc->cached_sectors = get_dev_size(hc->cached_dev);

	if (ti->len > hc->cached_sectors) {
		ti->error = "Device size larger than cached device";
		goto err;
	}

	INIT_DELAYED_WORK(&hc->dws, do_hsm);
	ti->split_io = hc->block_sectors;

	/* Set masks/shift for fast bio -> block mapping. */
	hc->offset_mask = ti->split_io - 1;

	spin_lock_init(&hc->provisioned_lock);
	hc->updates_since_last_commit = 0;
	smp_wmb();
	return 0;

err:
	hsm_dtr(ti);
	return r;
}

/* Map a hierarchical storage device  */
static int hsm_map(struct dm_target *ti, struct bio *bio,
		   union map_info *map_context)
{
	struct hsm_c *hc = ti->private;

	/* Don't bother the worker thread with read ahead io */
	if (bio_rw(bio) == READA)
		return -EIO;

	/* Remap sector to target begin. */
	bio->bi_sector -= ti->begin;
	map_context->ptr = (void*) bio->bi_destructor;

	spin_lock(&hc->lock);
	bio_list_add(&hc->in, bio);
	spin_unlock(&hc->lock);

	wake_do_hsm(hc);
	return DM_MAPIO_SUBMITTED;
}

/* End io process a bio. */
static int hsm_end_io(struct dm_target *ti, struct bio *bio,
		      int error, union map_info *map_context)
{
	struct hsm_block *b = (void*) bio->bi_destructor;

	/*
	 * Check for eventually delaying endio, because
	 * the metadata isn't written yet unless error.
	 */
	if (!error && test_bit(BLOCK_ACTIVE, &b->flags)) {
		spin_lock(&b->endio_lock);
		bio_list_add(&b->endio, bio);
		spin_unlock(&b->endio_lock);
		return DM_ENDIO_INCOMPLETE;
	}

	bio->bi_destructor = map_context->ptr;
	put_block(b);
	return error;
}

static void hsm_presuspend(struct dm_target *ti)
{
	struct hsm_c *hc = ti->private;

	set_bit(HC_BOUNCE_MODE, &hc->flags);
	cancel_delayed_work(&hc->dws);
	hsm_flush(ti);
	cancel_delayed_work(&hc->dws);
	requeue_all_bios(hc);
	clear_flush_blocks(hc);
DMINFO("%s", __func__);
	wait_event(hc->pending_block_io, list_empty(&hc->hsm_blocks));
}

static void hsm_postsuspend(struct dm_target *ti)
{
	struct hsm_c *hc = ti->private;

	hsm_metadata_close(hc->hmd);
	hc->hmd = NULL;
}

/*
 * Retrieves the number of blocks of the data device from
 * the superblock and compares it to the actual device size,
 * thus resizing the data device in case it has grown.
 *
 * This both copes with opening preallocated data devices in the ctr
 * being followed by a resume
 * -and-
 * calling the resume method individually after userpace has
 * grown the data device in reaction to a table event.
 */
static int hsm_preresume(struct dm_target *ti)
{
	int r;
	sector_t data_sectors;
	block_t data_blocks, sb_data_blocks;
	struct hsm_c *hc = ti->private;

	clear_bit(HC_BOUNCE_MODE, &hc->flags);
	clear_bit(HC_ERROR_EVENT, &hc->flags);

	if (!hc->hmd) {
		r = create_hsd(hc);
		if (r)
			return r;
	}

	data_sectors = get_dev_size(hc->data_dev);
	data_blocks = _sector_to_block(hc, data_sectors);
	r = hsm_metadata_get_data_dev_size(hc->hmd, hc->dev, &sb_data_blocks);
	if (r) {
		DMERR("failed to retrieve data device size");
		return r;
	}

	/* Nothing to resize. */
	if (data_blocks == sb_data_blocks)
		goto wake;

	if (data_blocks < sb_data_blocks) /* FIXME: weird */
		DMWARN("new data device size smaller than actual one");
	else {
		r = hsm_metadata_resize_data_dev(hc->hmd, hc->dev, data_blocks);
		if (r)
			DMERR("failed to resize data device");
		else {
			spin_lock(&hc->provisioned_lock);
			hc->data_sectors = data_sectors;
			hc->data_blocks = data_blocks;
			spin_unlock(&hc->provisioned_lock);
			goto wake;
		}
	}

	return 0;

wake:
	hc->flags = 0;
	wake_do_hsm(hc);
	return 0;
}

/* Thinp device status output method. */
static int hsm_status(struct dm_target *ti, status_type_t type,
			char *result, unsigned maxlen)
{
	ssize_t sz = 0;
	block_t allocated, data_blocks;
	char buf[BDEVNAME_SIZE];
	struct hsm_c *hc = ti->private;

	spin_lock(&hc->provisioned_lock);
	allocated = hc->provisioned_count + hc->allocations;
	data_blocks = hc->data_blocks;
	spin_unlock(&hc->provisioned_lock);

	switch (type) {
	case STATUSTYPE_INFO:
		/*   <chunks free> <chunks used> */
		DMEMIT("%llu %llu %d",
		       (LLU) data_blocks - allocated, (LLU) allocated,
		       atomic_read(&hc->dirty_blocks));
		break;

	case STATUSTYPE_TABLE:
		format_dev_t(buf, hc->cached_dev->bdev->bd_dev);
		DMEMIT("%s ", buf);
		format_dev_t(buf, hc->data_dev->bdev->bd_dev);
		DMEMIT("%s ", buf);
		format_dev_t(buf, hc->meta_dev->bdev->bd_dev);
		DMEMIT("%s %llu", buf, (LLU) hc->block_sectors);
	}

	return 0;
}

/* bvec merge method. */
static int hsm_bvec_merge(struct dm_target *ti,
			    struct bvec_merge_data *bvm,
			    struct bio_vec *biovec, int max_size)
{
	int r;
	unsigned long flags;
	struct hsm_c *hc = ti->private;
	struct request_queue *q = bdev_get_queue(_remap_dev(hc));
	block_t hsm_block, pool_block;

	if (!q->merge_bvec_fn)
		return max_size;

	bvm->bi_bdev = _remap_dev(hc);
	bvm->bi_sector -= ti->begin;
	hsm_block = _sector_to_block(hc, bvm->bi_sector);
	r = hsm_metadata_lookup(hc->hmd, hc->dev, hsm_block,
				0, &pool_block, &flags);
	if (r < 0)
		return 0;

	bvm->bi_sector = _remap_sector(hc, bvm->bi_sector, pool_block);
	return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

/* Provide io hints. */
static void
hsm_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct hsm_c *hc = ti->private;

	blk_limits_io_min(limits, 0);
	blk_limits_io_opt(limits, hc->block_sectors);
}

static int hsm_iterate_devices(struct dm_target *ti,
			       iterate_devices_callout_fn fn, void *data)
{
	struct hsm_c *hc = ti->private;

	return fn(ti, hc->cached_dev, 0, ti->len, data) ||
	       fn(ti, hc->data_dev,   0, hc->data_sectors, data);
}

/* Thinp device target interface. */
static struct target_type hsm_target = {
	.name = "hsm",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr = hsm_ctr,
	.dtr = hsm_dtr,
	.flush = hsm_flush,
	.map = hsm_map,
	.end_io = hsm_end_io,
	.presuspend = hsm_presuspend,
	.postsuspend = hsm_postsuspend,
	.preresume = hsm_preresume,
	.status = hsm_status,
	.merge = hsm_bvec_merge,
	.io_hints = hsm_io_hints,
	.iterate_devices = hsm_iterate_devices,
};

static int __init dm_hsm_init(void)
{
	int r;

	block_cache = KMEM_CACHE(hsm_block, 0);
        if (!block_cache) {
                DMERR("Couldn't create block cache.");
                return -ENOMEM;
        }

	srandom32(jiffies);
	r = dm_register_target(&hsm_target);
	if (r) {
		DMERR("Failed to register %s %s", DM_MSG_PREFIX, version);
		kmem_cache_destroy(block_cache);
	} else
		DMINFO("Registered %s %s", DM_MSG_PREFIX, version);

	return r;
}

static void dm_hsm_exit(void)
{
	dm_unregister_target(&hsm_target);
	kmem_cache_destroy(block_cache);
}

/* Module hooks */
module_init(dm_hsm_init);
module_exit(dm_hsm_exit);

MODULE_DESCRIPTION(DM_NAME "device-mapper hierachical storage target");
MODULE_AUTHOR("Heinz Mauelshagen <heinzm@redhat.com>");
MODULE_LICENSE("GPL");
