#ifndef DM_BLOCK_MANAGER_H
#define DM_BLOCK_MANAGER_H

#include <linux/blkdev.h>
#include <linux/types.h>
#include <linux/crc32c.h>

/*----------------------------------------------------------------*/

typedef uint64_t dm_block_t;

/* An opaque handle to a block of data */
struct dm_block;
dm_block_t dm_block_location(struct dm_block *b);
void *dm_block_data(struct dm_block *b);

static inline __le32 dm_block_csum_data(const void *data, unsigned int length)
{
	return __cpu_to_le32(crc32c(~(u32)0, data, length));
}

/*----------------------------------------------------------------*/

struct dm_block_manager;
struct dm_block_manager *
dm_block_manager_create(struct block_device *bdev, unsigned block_size,
			unsigned cache_size);
void dm_block_manager_destroy(struct dm_block_manager *bm);

unsigned dm_bm_block_size(struct dm_block_manager *bm);
dm_block_t dm_bm_nr_blocks(struct dm_block_manager *bm);

/*----------------------------------------------------------------*/

/* 4 bytes for CRC32c */
#define PERSISTENT_DATA_CSUM_SIZE 4

/*
 * The validator allows the caller to verify newly read data, and modify
 * the data just before writing.  eg, to calculate checksums.  It's
 * important to be consistent with your use of validators.  The only time
 * you can change validators is if you call dm_bm_write_lock_zero.
 */
struct dm_block_validator {
	const char *name;
	void (*prepare_for_write)(struct dm_block_validator *v, struct dm_block *b, size_t block_size);

	/* return 0 if valid, < 0 on error */
	int (*check)(struct dm_block_validator *v, struct dm_block *b, size_t block_size);
};

/*----------------------------------------------------------------*/

/*
 * You can have multiple concurrent readers, or a single writer holding a
 * block lock.
 */

/*
 * dm_bm_lock() locks a block, and returns via |data| a pointer to memory that
 * holds a copy of that block.  If you have write locked the block then any
 * changes you make to memory pointed to by |data| will be written back to
 * the disk sometime after dm_bm_unlock is called.
 */
int dm_bm_read_lock(struct dm_block_manager *bm, dm_block_t b,
		    struct dm_block_validator *v,
		    struct dm_block **result);

int dm_bm_write_lock(struct dm_block_manager *bm, dm_block_t b,
		     struct dm_block_validator *v,
		     struct dm_block **result);

/*
 * The *_try_lock variants return -EWOULDBLOCK if the block isn't
 * immediately available.
 */
int dm_bm_read_try_lock(struct dm_block_manager *bm, dm_block_t b,
			struct dm_block_validator *v,
			struct dm_block **result);

/*
 * dm_bm_write_lock_zero() is for use when you know you're going to completely
 * overwrite the block.  It saves a disk read.
 */
int dm_bm_write_lock_zero(struct dm_block_manager *bm, dm_block_t b,
			  struct dm_block_validator *v,
			  struct dm_block **result);

int dm_bm_unlock(struct dm_block *b);

/*
 * It's a common idiom to have a superblock that should be committed last.
 *
 * |superblock| should be write locked, it will be unlocked during this
 * function.  All dirty blocks are guaranteed to be written and flushed
 * before the superblock.
 *
 * This method always blocks.
 */
int dm_bm_flush_and_unlock(struct dm_block_manager *bm,
			   struct dm_block *superblock);

/*
 * The client may wish to change which block device the block manager
 * points at.  If you use this function then the cache remains intact, so
 * make sure your data is identical on the new device.  The new device must
 * be at least as long as the old.
 *
 * This function guarantees that once it returns, no further IO will occur
 * on the old device.
 */
int dm_bm_rebind_block_device(struct dm_block_manager *bm,
			      struct block_device *bdev);

/*
 * Debug routines.
 */
unsigned dm_bm_locks_held(struct dm_block_manager *bm);

/*----------------------------------------------------------------*/

#endif
