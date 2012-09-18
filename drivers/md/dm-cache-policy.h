/*
 * Copyright (C) 2012 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#ifndef DM_CACHE_POLICY_H
#define DM_CACHE_POLICY_H

#include "persistent-data/dm-block-manager.h"

/*----------------------------------------------------------------*/

/*
 * The cache policy makes the important decisions about which blocks get to
 * live on the faster cache device.
 *
 * When the core target has to remap a bio it calls the 'map' method of the
 * policy.  This returns an instruction telling the core target what to do.
 *
 * POLICY_HIT:
 *   That block is in the cache.  Remap to the cache and carry on.
 *
 * POLICY_MISS:
 *   This block is on the origin device.  Remap and carry on.
 *
 * POLICY_NEW:
 *   This block is currently on the origin device, but the policy wants to
 *   move it.  The core should:
 *
 *   - hold any further io to this origin block
 *   - copy the origin to the given cache block
 *   - release all the held blocks
 *   - remap the original block to the cache
 *
 * POLICY_REPLACE:
 *   This block is currently on the origin device.  The policy wants to
 *   move it to the cache, with the added complication that the destination
 *   cache block needs a writeback first.  The core should:
 *
 *   - hold any further io to this origin block
 *   - hold any further io to the origin block that's being written back
 *   - writeback
 *   - copy new block to cache
 *   - release held blocks
 *   - remap bio to cache and reissue.
 *
 * Should the core run into trouble while processing a POLICY_NEW or
 * POLICY_REPLACE instruction it will roll back the policies mapping using
 * remove_mapping() or force_mapping().  These methods must not fail.  This
 * approach avoids having transactional semantics in the policy (ie, the
 * core informing the policy when a migration is complete), and hence makes
 * it easier to write new policies.
 *
 * The policy methods should never block.  So be careful to implement using
 * bounded, preallocated memory.  Don't use mutexes etc.
 */
enum policy_operation {
	POLICY_HIT,
	POLICY_MISS,
	POLICY_NEW,
	POLICY_REPLACE
};

/*
 * This is the instruction passed back to the core target.
 */
struct policy_result {
	enum policy_operation op;
	dm_block_t old_oblock;	/* POLICY_REPLACE */
	dm_block_t cblock;	/* POLICY_HIT, POLICY_NEW, POLICY_REPLACE */
};

/*
 * The cache policy object.  Just a bunch of methods.  It is envisaged that
 * this structure will be embedded in a bigger, policy specific structure
 * (ie. use container_of()).
 */
struct dm_cache_policy {
	/*
	 * Destroys this object.
	 */
	void (*destroy)(struct dm_cache_policy *p);

	/*
	 * See large comment above.
	 *
	 * oblock      - the origin block we're interested in.
	 * can_migrate - gives permission for POLICY_NEW or POLICY_REPLACE
	 *               instructions.  If denied and the policy would have
	 *               returned one of these instructions it should
	 *               return -EWOULDBLOCK (FIXME: is there a better
	 *               error code).
	 *
	 * discarded_oblock - indicates whether the whole origin block is
	 *               in a discarded state.
	 * bio         - the bio that triggered this call.
	 * result      - gets filled in with the instruction.
	 *
	 * May only return 0, or -EWOULDBLOCK
	 */
	int (*map)(struct dm_cache_policy *p, dm_block_t oblock,
		   bool can_migrate, bool discarded_oblock,
		   struct bio *bio,
		   struct policy_result *result);

	/*
	 * Called when a cache target is first created.  Used to load a
	 * mapping from the metadata device into the policy.
	 */
	int (*load_mapping)(struct dm_cache_policy *p, dm_block_t oblock, dm_block_t cblock);

	/*
	 * Override functions used on the error paths of the core target.
	 * They must succeed.
	 */
	void (*remove_mapping)(struct dm_cache_policy *p, dm_block_t oblock);
	void (*force_mapping)(struct dm_cache_policy *p, dm_block_t current_oblock,
			      dm_block_t new_oblock);

	/*
	 * How full is the cache?
	 */
	dm_block_t (*residency)(struct dm_cache_policy *p);

	/*
	 * Because of where we sit in the block layer, we can be asked to
	 * map a lot of little bios that are all in the same block (no
	 * queue merging has occurred).  To stop the policy being fooled by
	 * these the core target sends regular tick() calls to the policy.
	 * The policy should only count an entry as hit once per tick.
	 */
	void (*tick)(struct dm_cache_policy *p);

	/*
	 * Book keeping ptr for the policy register, not for general use.
	 */
	void *private;
};

/*----------------------------------------------------------------*/

/*
 * We maintain a little register of the different policy types.
 */
#define CACHE_POLICY_NAME_MAX 16

struct dm_cache_policy_type {
	/* For use by the register code only. */
	struct list_head list;

	/*
	 * Policy writers should fill in these fields.  The name field is
	 * what gets passed on the target line to select your policy.
	 */
	char name[CACHE_POLICY_NAME_MAX];
	struct module *owner;
	struct dm_cache_policy *(*create)(dm_block_t cache_size);
};

int dm_cache_policy_register(struct dm_cache_policy_type *type);
void dm_cache_policy_unregister(struct dm_cache_policy_type *type);

/*----------------------------------------------------------------*/

#endif
