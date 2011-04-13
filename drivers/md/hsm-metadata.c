/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
 *
 * This file is released under the GPL.
 */

#include "hsm-metadata.h"
#include "persistent-data/transaction-manager.h"
#include "persistent-data/space-map-core.h"

#include <linux/list.h>
#include <linux/device-mapper.h>
#include <linux/workqueue.h>

/*----------------------------------------------------------------*/

#define	DM_MSG_PREFIX	"dm-hsm"
#define	DAEMON		DM_MSG_PREFIX	"d"

#define HSM_SUPERBLOCK_MAGIC 21081990
#define HSM_SUPERBLOCK_LOCATION 0
#define HSM_VERSION 1
#define HSM_METADATA_BLOCK_SIZE 4096
#define HSM_METADATA_CACHE_SIZE 128
#define SECTOR_TO_BLOCK_SHIFT 3

struct superblock {
	__le64 magic;
	__le64 version;

	__le64 metadata_block_size; /* in sectors */
	__le64 metadata_nr_blocks;

	__le64 data_block_size;	/* in sectors */
	__le64 data_nr_blocks;
	__le64 first_free_block;

	__le64 btree_root;

	/*
	 * Space map fields.
	 *
	 * The space map stores its root here, it will probably be longer
	 * than a __le64.
	 */
	__le64 sm_root_start;
};

/* FIXME: we need some locking */
struct hsm_metadata {
	atomic_t ref_count;
	struct hlist_node hash;

	struct block_device *bdev;
	struct block_manager *bm;
	struct space_map *sm;
	struct transaction_manager *tm;
	struct transaction_manager *nb_tm;

	/*
	 * Two level btree, first level is hsm_dev_t,
	 * second level mappings.
	 * I need a reverse mapping btree with the same info
	 * to be able to free cached blocks.
	 */
	struct btree_info info;

	/* non-blocking versions of the above */
	struct btree_info nb_info;

	/* just the top level, for deleting whole devices */
	struct btree_info dev_info;

	int have_inserted;

	struct rw_semaphore root_lock;
	struct block *sblock;
	block_t root;
	block_t reverse_root;

	struct workqueue_struct *wq;	/* Work queue. */
};

/*----------------------------------------------------------------*/

/* A little global cache of thinp metadata devs */
struct hsm_metadata;

/* FIXME: add a spin lock round the table */
#define TPM_TABLE_SIZE 1024
static struct hlist_head hsm_table_[TPM_TABLE_SIZE];

static void hsm_table_init(void)
{
	unsigned i;
	for (i = 0; i < TPM_TABLE_SIZE; i++)
		INIT_HLIST_HEAD(hsm_table_ + i);
}

static unsigned hash_bdev(struct block_device *bdev)
{
	/* FIXME: finish */
	/* bdev -> dev_t -> unsigned */
	return 0;
}

static void hsm_table_insert(struct hsm_metadata *hsm)
{
	unsigned bucket = hash_bdev(hsm->bdev);
	hlist_add_head(&hsm->hash, hsm_table_ + bucket);
}

static void hsm_table_remove(struct hsm_metadata *hsm)
{
	hlist_del(&hsm->hash);
}

static struct hsm_metadata *hsm_table_lookup(struct block_device *bdev)
{
	unsigned bucket = hash_bdev(bdev);
	struct hsm_metadata *hsm;
	struct hlist_node *n;

	hlist_for_each_entry (hsm, n, hsm_table_ + bucket, hash)
		if (hsm->bdev == bdev)
			return hsm;

	return NULL;
}

/*----------------------------------------------------------------*/

static int superblock_all_zeroes(struct block_manager *bm, int *result)
{
	int r, i;
	struct block *b;
	uint64_t *data;
	size_t block_size = bm_block_size(bm) / sizeof(uint64_t);

	r = bm_read_lock(bm, HSM_SUPERBLOCK_LOCATION, &b);
	if (r)
		return r;

	data = block_data(b);
	*result = 1;
	for (i = 0; i < block_size; i++) {
		if (data[i] != 0LL) {
			*result = 0;
			break;
		}
	}

	return bm_unlock(b);
}

static struct hsm_metadata *
alloc_(struct block_manager *bm, int create)
{
	int r;
	struct space_map *sm;
	struct transaction_manager *tm;
	struct hsm_metadata *hsm;
	struct block *sb;

	if (create) {
		r = tm_create_with_sm(bm, HSM_SUPERBLOCK_LOCATION, &tm, &sm, &sb);
		if (r < 0) {
			printk(KERN_ALERT "tm_create_with_sm failed");
			block_manager_destroy(bm);
			return NULL;
		}

		r = tm_pre_commit(tm);
		if (r < 0) {
			printk(KERN_ALERT "couldn't pre commit");
			goto bad;
		}

		r = tm_commit(tm, sb);
		if (r < 0) {
			printk(KERN_ALERT "couldn't commit");
			goto bad;
		}
	} else {
		struct superblock *s = NULL;

		r = tm_open_with_sm(bm, HSM_SUPERBLOCK_LOCATION,
				    (size_t) &((struct superblock *) NULL)->sm_root_start,
				    32, 	/* FIXME: magic number */
				    &tm, &sm, &sb);
		if (r < 0) {
			printk(KERN_ALERT "tm_open_with_sm failed");
			block_manager_destroy(bm);
			return NULL;
		}

		s = block_data(sb);
		if (__le64_to_cpu(s->magic) != HSM_SUPERBLOCK_MAGIC) {
			printk(KERN_ALERT "hsm-metadata superblock is invalid");
			goto bad;
		}

		tm_unlock(tm, sb);
	}

	hsm = kmalloc(sizeof(*hsm), GFP_KERNEL);
	if (!hsm) {
		printk(KERN_ALERT "hsm-metadata could not allocate metadata struct");
		goto bad;
	}

	hsm->bm = bm;
	hsm->sm = sm;
	hsm->tm = tm;
	hsm->nb_tm = tm_create_non_blocking_clone(tm);
	if (!hsm->nb_tm) {
		printk(KERN_ALERT "hsm-metadata could not create clone tm");
		goto bad;
	}

	hsm->sblock = NULL;
	hsm->info.tm = tm;
	hsm->info.levels = 2;

	hsm->info.value_type.context = NULL;
	hsm->info.value_type.size = sizeof(block_t);
	hsm->info.value_type.copy = NULL; /* because the blocks are held in a separate device */
	hsm->info.value_type.del = NULL;
	hsm->info.value_type.equal = NULL;

	memcpy(&hsm->nb_info, &hsm->info, sizeof(hsm->nb_info));
	hsm->nb_info.tm = hsm->nb_tm;

	hsm->dev_info.tm = tm;
	hsm->dev_info.levels = 1;
	hsm->dev_info.value_type.context = tm;
	hsm->dev_info.value_type.copy = NULL; /* FIXME: finish */
	hsm->dev_info.value_type.del = NULL;
	hsm->dev_info.value_type.equal = NULL;

	hsm->have_inserted = 0;
	hsm->root = 0;

	init_rwsem(&hsm->root_lock);

	/* Create singlethreaded workqueue that will service all devices
	 * that use this metadata.
	 */
	hsm->wq = alloc_ordered_workqueue(DAEMON, WQ_MEM_RECLAIM);
	if (!hsm->wq) {
		printk(KERN_ALERT "couldn't create workqueue for metadata object");
		goto bad;
	}

	return hsm;

bad:
	tm_destroy(tm);
	sm_destroy(sm);
	block_manager_destroy(bm);
	return NULL;
}

static int hsm_metadata_begin(struct hsm_metadata *hsm)
{
	int r;
	struct superblock *s;

	BUG_ON(hsm->sblock);
	hsm->have_inserted = 0;
	r = bm_write_lock(hsm->bm, HSM_SUPERBLOCK_LOCATION, &hsm->sblock);
	if (r)
		return r;

	s = (struct superblock *) block_data(hsm->sblock);
	hsm->root = __le64_to_cpu(s->btree_root);
	return 0;
}

static struct hsm_metadata *
hsm_metadata_open_(struct block_device *bdev,
		     sector_t data_block_size,
		     block_t data_dev_size)
{
	int r;
	struct superblock *sb;
	struct hsm_metadata *hsm;
	sector_t bdev_size = i_size_read(bdev->bd_inode) >> SECTOR_SHIFT;
	struct block_manager *bm;
	int create;

	bm = block_manager_create(bdev,
				  HSM_METADATA_BLOCK_SIZE,
				  HSM_METADATA_CACHE_SIZE);
	if (!bm) {
		printk(KERN_ALERT "hsm-metadata could not create block manager");
		return NULL;
	}

	r = superblock_all_zeroes(bm, &create);
	if (r) {
		block_manager_destroy(bm);
		return NULL;
	}

	hsm = alloc_(bm, create);
	if (!hsm)
		return NULL;
	hsm->bdev = bdev;

	if (create) {
		if (!hsm->sblock) {
			r = hsm_metadata_begin(hsm);
			if (r < 0)
				goto bad;
		}

		sb = (struct superblock *) block_data(hsm->sblock);
		sb->magic = __cpu_to_le64(HSM_SUPERBLOCK_MAGIC);
		sb->version = __cpu_to_le64(HSM_VERSION);
		sb->metadata_block_size = __cpu_to_le64(1 << SECTOR_TO_BLOCK_SHIFT);
		sb->metadata_nr_blocks = __cpu_to_le64(bdev_size >> SECTOR_TO_BLOCK_SHIFT);
		sb->data_block_size = __cpu_to_le64(data_block_size);
		sb->data_nr_blocks = __cpu_to_le64(data_dev_size);
		sb->first_free_block = 0;

		r = btree_empty(&hsm->info, &hsm->root);
		if (r < 0)
			goto bad;

		r = btree_empty(&hsm->info, &hsm->reverse_root);
		if (r < 0) {
			btree_del(&hsm->info, hsm->root);
			goto bad;
		}

		hsm->have_inserted = 1;
		r = hsm_metadata_commit(hsm);
		if (r < 0)
			goto bad;
	} else {
		r = hsm_metadata_begin(hsm);
		if (r < 0)
			goto bad;
	}

	return hsm;

bad:
	hsm_metadata_close(hsm);
	return NULL;
}

struct hsm_metadata *
hsm_metadata_open(struct block_device *bdev,
		     sector_t data_block_size,
		     block_t data_dev_size)
{
	struct hsm_metadata *hsm;

	hsm = hsm_table_lookup(bdev);
	if (hsm)
		atomic_inc(&hsm->ref_count);
	else {
		hsm = hsm_metadata_open_(bdev, data_block_size, data_dev_size);
		atomic_set(&hsm->ref_count, 1);
		hsm_table_insert(hsm);
	}

	BUG_ON(!hsm->sblock);
	return hsm;
}
EXPORT_SYMBOL_GPL(hsm_metadata_open);

static void print_sblock(struct superblock *sb)
{
#if 0
	printk(KERN_ALERT "magic = %u", (unsigned) __le64_to_cpu(sb->magic));
	printk(KERN_ALERT "version = %u", (unsigned) __le64_to_cpu(sb->version));
	printk(KERN_ALERT "md block size = %u", (unsigned) __le64_to_cpu(sb->metadata_block_size));
	printk(KERN_ALERT "md nr blocks = %u", (unsigned) __le64_to_cpu(sb->metadata_nr_blocks));
	printk(KERN_ALERT "data block size = %u", (unsigned) __le64_to_cpu(sb->data_block_size));
	printk(KERN_ALERT "data nr blocks = %u", (unsigned) __le64_to_cpu(sb->data_nr_blocks));
	printk(KERN_ALERT "first free = %u", (unsigned) __le64_to_cpu(sb->first_free_block));
	printk(KERN_ALERT "btree root = %u", (unsigned) __le64_to_cpu(sb->btree_root));
	printk(KERN_ALERT "sm nr blocks = %u", (unsigned) __le64_to_cpu(sb->sm_root_start));
	printk(KERN_ALERT "bitmap root = %u", (unsigned) __le64_to_cpu(
		       *(((__le64 *) &sb->sm_root_start) + 1)
		       ));
	printk(KERN_ALERT "ref count root = %u",(unsigned) __le64_to_cpu(
		       *(((__le64 *) &sb->sm_root_start) + 2)));
#endif
}

void
hsm_metadata_close(struct hsm_metadata *hsm)
{
	if (atomic_dec_and_test(&hsm->ref_count)) {
		printk(KERN_ALERT "destroying hsm");
		hsm_table_remove(hsm);

		if (hsm->sblock)
			hsm_metadata_commit(hsm);

		tm_destroy(hsm->tm);
		tm_destroy(hsm->nb_tm);
		block_manager_destroy(hsm->bm);
		sm_destroy(hsm->sm);

		if (hsm->wq)
			destroy_workqueue(hsm->wq);

		kfree(hsm);
	}
}
EXPORT_SYMBOL_GPL(hsm_metadata_close);

int hsm_metadata_commit(struct hsm_metadata *hsm)
{
	int r;
	size_t len;

	if (!hsm->have_inserted)
		/* if nothing's been inserted, then nothing has changed */
		return 0;

	down_write(&hsm->root_lock);
	r = tm_pre_commit(hsm->tm);
	if (r < 0) {
		up_write(&hsm->root_lock);
		return r;
	}

	r = sm_root_size(hsm->sm, &len);
	if (r < 0) {
		up_write(&hsm->root_lock);
		return r;
	}

	{
		struct superblock *sb = block_data(hsm->sblock);
		sb->btree_root = __cpu_to_le64(hsm->root);
		r = sm_copy_root(hsm->sm, &sb->sm_root_start, len);
		if (r < 0) {
			up_write(&hsm->root_lock);
			return r;
		}

		print_sblock(sb);
	}

	r = tm_commit(hsm->tm, hsm->sblock);

	/* open the next transaction */
	hsm->sblock = NULL;
	r = hsm_metadata_begin(hsm); /* FIXME: the semantics of failure are confusing here, probably have to make begin a public method again */
	up_write(&hsm->root_lock);

	return r;
}
EXPORT_SYMBOL_GPL(hsm_metadata_commit);

void split_result(block_t result, block_t *b, unsigned long *flags)
{
	*b = result & 0xFFFFFFFFFFFFFFF;
	*flags = (result & 0xF000000000000000) >> 60;
}

int hsm_metadata_insert(struct hsm_metadata *hsm,
			hsm_dev_t dev,
			block_t hsm_block,
			block_t *pool_block,
		        unsigned long *flags)
{
	int r;
	struct superblock *sb;
	block_t b, dummy, nr_blocks, keys[2];

	keys[0] = dev;
	keys[1] = hsm_block;

	down_write(&hsm->root_lock);
	hsm->have_inserted = 1;
	sb = block_data(hsm->sblock);
	nr_blocks = __le64_to_cpu(sb->data_nr_blocks);
	b = __le64_to_cpu(sb->first_free_block);

	if (b >= nr_blocks) {
		/* we've run out of space, client should extend and then retry */
		printk(KERN_ALERT "out of thinp data space");
		up_write(&hsm->root_lock);
		return -ENOSPC;
	}

	/* Block may no be interfearing with flags in the high bits. */
	split_result(b, &dummy, flags);
	BUG_ON(*flags);

	r = btree_insert(&hsm->info, hsm->root, keys, &b, &hsm->root);
	if (!r) {
		keys[1] = b;
		r = btree_insert(&hsm->info, hsm->reverse_root,
				 keys, &hsm_block, &hsm->reverse_root);
	}

	up_write(&hsm->root_lock);

	*pool_block = b;
	*flags = 0;

	sb->first_free_block = __cpu_to_le64(b + 1);
	if (r < 0)
		return r;

	return 0;
}
EXPORT_SYMBOL_GPL(hsm_metadata_insert);

int hsm_metadata_remove(struct hsm_metadata *hsm,
			hsm_dev_t dev,
			block_t hsm_block)
{
	int r;
	unsigned long dummy;
	block_t keys[2], pool_block;

	/* Mapping has to exists on update. */
	r = hsm_metadata_lookup(hsm, dev, hsm_block, 1, &pool_block, &dummy);
	if (r < 0)
		return r;

	keys[0] = dev;
	keys[1] = hsm_block;

	down_write(&hsm->root_lock);
	btree_remove(&hsm->info, hsm->root, keys, &hsm->root);
	keys[1] = pool_block;
	btree_remove(&hsm->info, hsm->reverse_root, keys,
		     &hsm->reverse_root);
	up_write(&hsm->root_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(hsm_metadata_remove);

int
hsm_metadata_lookup(struct hsm_metadata *hsm,
		    hsm_dev_t dev,
		    block_t hsm_block,
		    int can_block,
		    block_t *pool_block, unsigned long *flags)
{
	int r;
	block_t keys[2], result;

	keys[0] = dev;
	keys[1] = hsm_block;

	if (can_block) {
		down_read(&hsm->root_lock);
		r = btree_lookup_equal(&hsm->info, hsm->root, keys, &result);
		up_read(&hsm->root_lock);

	} else if (down_read_trylock(&hsm->root_lock)) {
		r = btree_lookup_equal(&hsm->nb_info, hsm->root, keys, &result);
		up_read(&hsm->root_lock);

	} else
		r = -EWOULDBLOCK;

	if (!r)
		split_result(result, pool_block, flags);

	return r;
}
EXPORT_SYMBOL_GPL(hsm_metadata_lookup);

int hsm_metadata_update(struct hsm_metadata *hsm,
			hsm_dev_t dev,
			block_t hsm_block,
		        unsigned long flags)
{
	int r;
	unsigned long dummy;
	block_t keys[2], pool_block;

	/* Mapping has to exists on update. */
	r = hsm_metadata_lookup(hsm, dev, hsm_block, 1, &pool_block, &dummy);
	if (r < 0)
		return r;

	keys[0] = dev;
	keys[1] = hsm_block;

	down_write(&hsm->root_lock);
	hsm->have_inserted = 1;
	pool_block |= (flags << 60);
	r = btree_insert(&hsm->info, hsm->root, keys, &pool_block, &hsm->root);
	up_write(&hsm->root_lock);

	return r < 0 ? r : 0;
}
EXPORT_SYMBOL_GPL(hsm_metadata_update);

int
hsm_metadata_lookup_reverse(struct hsm_metadata *hsm,
			    hsm_dev_t dev,
			    block_t pool_block,
			    int can_block,
			    block_t *result)
{
	int r;
	block_t keys[2];

	keys[0] = dev;
	keys[1] = pool_block;

	if (can_block) {
		down_read(&hsm->root_lock);
		r = btree_lookup_equal(&hsm->info, hsm->reverse_root, keys, result);
		up_read(&hsm->root_lock);

	} else if (down_read_trylock(&hsm->root_lock)) {
		r = btree_lookup_equal(&hsm->nb_info, hsm->reverse_root, keys, result);
		up_read(&hsm->root_lock);

	} else
		r = -EWOULDBLOCK;

	return r;
}
EXPORT_SYMBOL_GPL(hsm_metadata_lookup_reverse);

int
hsm_metadata_delete(struct hsm_metadata *hsm,
		      hsm_dev_t dev)
{
	printk(KERN_ALERT "requested deletion of %u", (unsigned) dev);
	down_write(&hsm->root_lock);
	// FIXME: finish
	up_write(&hsm->root_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(hsm_metadata_delete);

int
hsm_metadata_get_data_block_size(struct hsm_metadata *hsm,
				   hsm_dev_t dev,
				   sector_t *result)
{
	down_read(&hsm->root_lock);
	{
		struct superblock *sb = (struct superblock *) block_data(hsm->sblock);
		*result = __le64_to_cpu(sb->data_block_size);
	}
	up_read(&hsm->root_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(hsm_metadata_get_data_block_size);

int
hsm_metadata_get_data_dev_size(struct hsm_metadata *hsm,
				 hsm_dev_t dev,
				 block_t *result)
{
	down_read(&hsm->root_lock);
	{
		struct superblock *sb = (struct superblock *) block_data(hsm->sblock);
		*result = __le64_to_cpu(sb->data_nr_blocks);
	}
	up_read(&hsm->root_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(hsm_metadata_get_data_dev_size);

int
hsm_metadata_get_provisioned_blocks(struct hsm_metadata *hsm,
				      hsm_dev_t dev,
				      block_t *result)
{
	down_read(&hsm->root_lock);
	{
		struct superblock *sb = (struct superblock *) block_data(hsm->sblock);
		*result = __le64_to_cpu(sb->first_free_block);
	}
	up_read(&hsm->root_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(hsm_metadata_get_provisioned_blocks);

int
hsm_metadata_resize_data_dev(struct hsm_metadata *hsm,
			       hsm_dev_t dev,
			       block_t new_size)
{
	block_t b;

	down_write(&hsm->root_lock);
	{
		struct superblock *sb = (struct superblock *) block_data(hsm->sblock);

		b = __le64_to_cpu(sb->first_free_block);
		if (b > new_size) {
			/* this would truncate mapped blocks */
			up_write(&hsm->root_lock);
			return -ENOSPC;
		}

		sb->data_nr_blocks = __cpu_to_le64(new_size);
	}
	up_write(&hsm->root_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(hsm_metadata_resize_data_dev);

struct workqueue_struct *
hsm_metadata_get_workqueue(struct hsm_metadata *hsm)
{
	return hsm->wq;
}
EXPORT_SYMBOL_GPL(hsm_metadata_get_workqueue);

static int hsm_metadata_init(void)
{
	hsm_table_init();
	return 0;
}

static void hsm_metadata_exit(void)
{
}

module_init(hsm_metadata_init);
module_exit(hsm_metadata_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joe Thornber");
MODULE_DESCRIPTION("Metadata manager for thin provisioning dm target");

/*----------------------------------------------------------------*/
