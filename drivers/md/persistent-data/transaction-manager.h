#ifndef SNAPSHOTS_TRANSACTION_MANAGER_H
#define SNAPSHOTS_TRANSACTION_MANAGER_H

#include "block-manager.h"
#include "space-map.h"

/*----------------------------------------------------------------*/

/*
 * This manages the scope of a transaction.  It also enforces immutability
 * of the on-disk data structures by limiting access to writeable blocks.
 *
 * Clients should not fiddle with the block manager directly.
 */
struct transaction_manager;

struct transaction_manager *tm_create(struct block_manager *bm,
				      struct space_map *sm);
void tm_destroy(struct transaction_manager *tm);

/*
 * The non-blocking version of a transaction manager is intended for use in
 * fast path code that needs to do lookups.  eg, a dm mapping function.
 * You create the non-blocking variant from a normal tm.  The interface is
 * the same, except that most functions will just return -EWOULDBLOCK.
 * Call tm_destroy() as you would with a normal tm when you've finished
 * with it.  You may not destroy the original prior to clones.
 */
struct transaction_manager *tm_create_non_blocking_clone(struct transaction_manager *real);

/*
 * The client may want to manage some blocks directly (eg, the
 * superblocks).  Call this immediately after construction to reserve
 * blocks.
 */
int tm_reserve_block(struct transaction_manager *tm, block_t b);

int tm_begin(struct transaction_manager *tm);

/*
 * We use a 2 phase commit here.
 *
 * i) In the first phase the block manager is told to start flushing, and
 * the changes to the space map are written to disk.  You should interogate
 * your particular space map to get detail of its root node etc. to be
 * included in your superblock.
 *
 * ii) |root| will be committed last.  You shouldn't use more than the
 * first 512 bytes of |root| if you wish the transaction to survive a power
 * failure.  You *must* have a write lock held on |root| for both stage (i)
 * and (ii).  The commit will drop the write lock.
 */
int tm_pre_commit(struct transaction_manager *tm);
int tm_commit(struct transaction_manager *tm, struct block *root);

/*
 * These methods are the only way to get hold of a writeable block.
 *
 * tm_new_block() is pretty self explanatory.  Make sure you do actually
 * write to the whole of |data| before you unlock, otherwise you could get
 * a data leak.  (The other option is for tm_new_block() to zero new blocks
 * before handing them out, which will be redundant in most if not all
 * cases).
 *
 * tm_shadow_block() will allocate a new block and copy the data from orig
 * to it.  It then decrements the reference count on original block.  Use
 * this to update the contents of a block in a data structure, don't
 * confuse this with a clone - you shouldn't access the orig block after
 * this operation.  Because the tm knows the scope of the transaction it
 * can optimise requests for a shadow of a shadow to a no-op.  Don't forget
 * to unlock when you've finished with the shadow.
 *
 * The |inc_children| flag is used to tell the caller whether they need to
 * adjust reference counts for children (data in the block may refer to
 * other blocks).
 */
int tm_alloc_block(struct transaction_manager *tm, block_t *new);

/* zeroes the new block at returns with write lock held */
int tm_new_block(struct transaction_manager *tm, struct block **result);

/*
 * Shadowing implicitly drops a reference on |orig|, so you must not have
 * it locked when you call this.
 */
int tm_shadow_block(struct transaction_manager *tm,
		    block_t orig,
		    struct block **result,
		    int *inc_children);

/*
 * Read access.  You can lock any block you want, if there's a write lock
 * on it outstanding then it'll block.
 */
int tm_read_lock(struct transaction_manager *tm, block_t b, struct block **result);
int tm_unlock(struct transaction_manager *tm, struct block *b);

/*
 * Functions for altering the reference count of a block directly.
 */
void tm_inc(struct transaction_manager *tm, block_t b);
void tm_dec(struct transaction_manager *tm, block_t b);
int tm_ref(struct transaction_manager *tm, block_t b, uint32_t *result);

struct block_manager *tm_get_bm(struct transaction_manager *tm);

/*
 * A little utility that ties the knot by producing a transaction manager
 * that has a space map managed by the transaction manager ...
 *
 * Returns a tm that has an open transaction to write the new disk sm.
 * Caller should store the new sm root and commit.
 */
int tm_create_with_sm(struct block_manager *bm,
		      block_t superblock,
		      struct transaction_manager **tm,
		      struct space_map **sm,
		      struct block **sb);

int tm_open_with_sm(struct block_manager *bm,
		    block_t superblock,
		    size_t root_offset,
		    size_t root_max_len,
		    struct transaction_manager **tm,
		    struct space_map **sm,
		    struct block **sb);

/* useful for debugging performance */
unsigned tm_shadow_count(struct transaction_manager *tm);

/*----------------------------------------------------------------*/

#endif
