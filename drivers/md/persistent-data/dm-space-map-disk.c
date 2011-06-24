#include <linux/list.h>
#include <linux/slab.h>
#include <asm-generic/bitops/le.h>

#include "dm-space-map-disk.h"
#include "dm-btree-internal.h"

/*----------------------------------------------------------------
 * Low level disk format
 *
 * We hold 2 bits per block, which represent UNUSED = 0, REF_COUNT = 1,
 * REF_COUNT = 2 and REF_COUNT = many.  A separate btree holds ref counts
 * for blocks that are over 2.
 *--------------------------------------------------------------*/
struct ll_disk {
	struct dm_transaction_manager *tm;
	struct dm_btree_info bitmap_info;
	struct dm_btree_info ref_count_info;

	uint32_t block_size;
	uint32_t entries_per_block;
	dm_block_t nr_blocks;
	dm_block_t nr_allocated;
	dm_block_t bitmap_root;
	dm_block_t ref_count_root;

	dm_block_t bitmap_index[4096]; /* FIXME: magic number */
};

struct index_entry {
	__le64 blocknr;
	__le32 nr_free;
	__le32 none_free_before;
}  __attribute__ ((packed));

struct sm_root {
	__le64 nr_blocks;
	__le64 nr_allocated;
	__le64 bitmap_root;
	__le64 ref_count_root;
};

#define ENTRIES_PER_BYTE 4
#define ENTRIES_PER_WORD 32

/*----------------------------------------------------------------*/

static uint64_t div_up(uint64_t v, uint64_t n)
{
	uint64_t t = v;
	uint64_t rem = do_div(t, n);
	return t + (rem > 0 ? 1 : 0);
}

static uint64_t mod64(uint64_t n, uint64_t d)
{
	return do_div(n, d);
}

/*----------------------------------------------------------------
 * bitmap validator
 *--------------------------------------------------------------*/

static void bitmap_prepare_for_write(struct dm_block_validator *v,
				     struct dm_block *b)
{
}

static int bitmap_check(struct dm_block_validator *v,
			struct dm_block *b)
{
	return 0;
}

static struct dm_block_validator bitmap_validator_ = {
	.prepare_for_write = bitmap_prepare_for_write,
	.check = bitmap_check
};

/*----------------------------------------------------------------*/

static unsigned __lookup_bitmap(void *addr, dm_block_t b)
{
	unsigned val = 0;
	__le64 *words = (__le64 *) addr;
	__le64 *w = words + (b / ENTRIES_PER_WORD); /* FIXME: 64 bit div, use shift */
	b %= ENTRIES_PER_WORD;

	val = test_bit_le(b * 2, (void *) w) ? 1 : 0;
	val <<= 1;
	val |= test_bit_le(b * 2 + 1, (void *) w) ? 1 : 0;
	return val;
}

static void __set_bitmap(void *addr, dm_block_t b, unsigned val)
{
	__le64 *words = (__le64 *) addr;
	__le64 *w = words + (b / ENTRIES_PER_WORD); /* FIXME: use shift */
	b %= ENTRIES_PER_WORD;

	if (val & 2)
		__set_bit_le(b * 2, (void *) w);
	else
		__clear_bit_le(b * 2, (void *) w);

	if (val & 1)
		__set_bit_le(b * 2 + 1, (void *) w);
	else
		__clear_bit_le(b * 2 + 1, (void *) w);
}

static int ie_find_free(void *addr, unsigned begin, unsigned end,
			unsigned *result)
{
	/* FIXME: slow, find a quicker way in Hackers Delight */
	while (begin < end) {
		if (!__lookup_bitmap(addr, begin)) {
			*result = begin;
			return 0;
		}
		begin++;
	}

	return -ENOSPC;
}

static int ll_init(struct ll_disk *io, struct dm_transaction_manager *tm)
{
	io->tm = tm;
	io->bitmap_info.tm = tm;
	io->bitmap_info.levels = 1;

	/*
	 * Because the new bitmap blocks are created via a shadow
	 * operation, the old entry has already had it's reference count
	 * decremented.  So we don't need the btree to do any book
	 * keeping.
	 */
	io->bitmap_info.value_type.size = sizeof(struct index_entry);
	io->bitmap_info.value_type.copy = NULL;
	io->bitmap_info.value_type.del = NULL;
	io->bitmap_info.value_type.equal = NULL;

	io->ref_count_info.tm = tm;
	io->ref_count_info.levels = 1;
	io->ref_count_info.value_type.size = sizeof(uint32_t);
	io->ref_count_info.value_type.copy = NULL;
	io->ref_count_info.value_type.del = NULL;
	io->ref_count_info.value_type.equal = NULL;

	io->block_size = dm_bm_block_size(dm_tm_get_bm(tm));

	if (io->block_size > (1 << 30)) {
		printk(KERN_ALERT "block size too big to hold bitmaps");
		return -EINVAL;
	}
	io->entries_per_block = io->block_size * ENTRIES_PER_BYTE;
	io->nr_blocks = 0;
	io->bitmap_root = 0;
	io->ref_count_root = 0;

	return 0;
}

static int ll_new(struct ll_disk *io, struct dm_transaction_manager *tm,
		  dm_block_t nr_blocks)
{
	int r;
	dm_block_t i;
	unsigned blocks;

	r = ll_init(io, tm);
	if (r < 0)
		return r;

	io->nr_blocks = nr_blocks;
	io->nr_allocated = 0;
	r = dm_btree_empty(&io->bitmap_info, &io->bitmap_root);
	if (r < 0)
		return r;

	blocks = div_up(nr_blocks, io->entries_per_block);
	for (i = 0; i < blocks; i++) {
		struct dm_block *b;
		struct index_entry idx;

		r = dm_tm_new_block(tm, &bitmap_validator_, &b);
		if (r < 0)
			return r;
		printk(KERN_ALERT "sm-disk allocated bitmap at: %u", (unsigned) dm_block_location(b));
		idx.blocknr = __cpu_to_le64(dm_block_location(b));

		r = dm_tm_unlock(tm, b);
		if (r < 0)
			return r;

		idx.nr_free = __cpu_to_le32(io->entries_per_block);
		idx.none_free_before = 0;

		r = dm_btree_insert(&io->bitmap_info, io->bitmap_root,
				    &i, &idx, &io->bitmap_root);
		if (r < 0)
			return r;
	}

	r = dm_btree_empty(&io->ref_count_info, &io->ref_count_root);
	if (r < 0) {
		dm_btree_del(&io->bitmap_info, io->bitmap_root);
		return r;
	}

	return 0;
}

static int ll_open(struct ll_disk *ll, struct dm_transaction_manager *tm,
		   void *root, size_t len)
{
	int r;
	struct sm_root *smr = (struct sm_root *) root;

	if (len < sizeof(struct sm_root)) {
		printk(KERN_ALERT "sm_disk root too small");
		return -ENOMEM;
	}

	r = ll_init(ll, tm);
	if (r < 0)
		return r;

	ll->nr_blocks = __le64_to_cpu(smr->nr_blocks);
	ll->nr_allocated = __le64_to_cpu(smr->nr_allocated);
	ll->bitmap_root = __le64_to_cpu(smr->bitmap_root);
	ll->ref_count_root = __le64_to_cpu(smr->ref_count_root);

	return 0;
}

static int ll_lookup_bitmap(struct ll_disk *io, dm_block_t b, uint32_t *result)
{
	int r;
	dm_block_t index = b;
	struct index_entry ie;
	struct dm_block *blk;

	do_div(index, io->entries_per_block);

	r = dm_btree_lookup(&io->bitmap_info, io->bitmap_root, &index, &ie);
	if (r < 0)
		return r;

	r = dm_tm_read_lock(io->tm, __le64_to_cpu(ie.blocknr), &bitmap_validator_, &blk);
	if (r < 0)
		return r;
	*result = __lookup_bitmap(dm_block_data(blk),
				  mod64(b, io->entries_per_block));
	return dm_tm_unlock(io->tm, blk);
}

static int ll_lookup(struct ll_disk *io, dm_block_t b, uint32_t *result)
{
	int r = ll_lookup_bitmap(io, b, result);

	if (r)
		return r;

	if (*result == 3) {
		__le32 le_rc;
		r = dm_btree_lookup(&io->ref_count_info, io->ref_count_root,
				    &b, &le_rc);
		if (r < 0)
			return r;

		*result = __le32_to_cpu(le_rc);
	}

	return r;
}

static int ll_find_free_block(struct ll_disk *io, dm_block_t begin,
			      dm_block_t end, dm_block_t *result)
{
	int r;
	struct index_entry ie;
	dm_block_t i, index_begin = begin;
	dm_block_t index_end = div_up(end, io->entries_per_block);

	do_div(index_begin, io->entries_per_block);
	for (i = index_begin; i < index_end; i++, begin = 0) {
		r = dm_btree_lookup(&io->bitmap_info, io->bitmap_root, &i, &ie);
		if (r < 0)
			return r;

		if (__le32_to_cpu(ie.nr_free) > 0) {
			struct dm_block *blk;
			unsigned position;
			uint32_t bit_end = (i == index_end - 1) ?
				mod64(end, io->entries_per_block) :
				io->entries_per_block;

			r = dm_tm_read_lock(io->tm, __le64_to_cpu(ie.blocknr),
					    &bitmap_validator_, &blk);
			if (r < 0)
				return r;

			r = ie_find_free(dm_block_data(blk),
					 mod64(begin, io->entries_per_block),
					 bit_end, &position);
			if (r < 0) {
				dm_tm_unlock(io->tm, blk);
				return r;
			}

			r = dm_tm_unlock(io->tm, blk);
			if (r < 0)
				return r;

			*result = i * io->entries_per_block + (dm_block_t) position;
			return 0;
		}
	}

	return -ENOSPC;
}

static int ll_insert(struct ll_disk *io, dm_block_t b, uint32_t ref_count)
{
	int r;
	uint32_t bit, old;
	struct dm_block *nb;
	dm_block_t index = b;
	struct index_entry ie;
	void *bm;
	int inc;

	do_div(index, io->entries_per_block);
	r = dm_btree_lookup(&io->bitmap_info, io->bitmap_root, &index, &ie);
	if (r < 0)
		return r;

	r = dm_tm_shadow_block(io->tm, __le64_to_cpu(ie.blocknr),
			       &bitmap_validator_, &nb, &inc);
	if (r < 0) {
		printk(KERN_ALERT "shadow failed");
		return r;
	}

	bm = dm_block_data(nb);
	bit = mod64(b, io->entries_per_block);
	old = __lookup_bitmap(bm, bit);

	if (ref_count <= 2) {
		__set_bitmap(bm, bit, ref_count);
		BUG_ON(__lookup_bitmap(bm, bit) != ref_count);

		if (old > 2) {
#if 0
			if (!btree_remove(&io->ref_count_info, io->ref_count_root,
					  &b, &io->ref_count_root))
				abort();
#endif
		}
	} else {
		__le32 le_rc = __cpu_to_le32(ref_count);
		__set_bitmap(bm, bit, 3);
		r = dm_btree_insert(&io->ref_count_info, io->ref_count_root,
				    &b, &le_rc, &io->ref_count_root);
		if (r < 0) {
			dm_tm_unlock(io->tm, nb);
			/* FIXME: release shadow? or assume the whole transaction will be ditched */
			printk(KERN_ALERT "ref count insert failed");
			return r;
		}
	}

	r = dm_tm_unlock(io->tm, nb);
	if (r < 0)
		return r;

	if (ref_count && !old) {
		io->nr_allocated++;
		ie.nr_free = __cpu_to_le32(__le32_to_cpu(ie.nr_free) - 1);
		if (__le32_to_cpu(ie.none_free_before) == b)
			ie.none_free_before = __cpu_to_le32(b + 1);

	} else if (old && !ref_count) {
		io->nr_allocated--;
		ie.nr_free = __cpu_to_le32(__le32_to_cpu(ie.nr_free) + 1);
		ie.none_free_before = __cpu_to_le32(min((dm_block_t) __le32_to_cpu(ie.none_free_before), b));
	}

	ie.blocknr = __cpu_to_le64(dm_block_location(nb));
	r = dm_btree_insert(&io->bitmap_info, io->bitmap_root,
			    &index, &ie, &io->bitmap_root);
	if (r < 0)
		return r;

	return 0;
}

static int ll_inc(struct ll_disk *ll, dm_block_t b)
{
	int r;
	uint32_t rc;

	r = ll_lookup(ll, b, &rc);
	if (r)
		return r;

	return ll_insert(ll, b, rc + 1);
}

static int ll_dec(struct ll_disk *ll, dm_block_t b)
{
	int r;
	uint32_t rc;

	r = ll_lookup(ll, b, &rc);
	if (r)
		return r;

	if (!rc)
		return -EINVAL;

	return ll_insert(ll, b, rc - 1);
}

/*----------------------------------------------------------------
 * Space map interface.
 *
 * The low level disk format is written using the standard btree and
 * transaction manager.  This means that performing disk operations may
 * cause us to recurse into the space map in order to allocate new blocks.
 * For this reason we have a pool of pre-allocated blocks large enough to
 * service any ll_disk operation.
 *--------------------------------------------------------------*/

/*
 * FIXME: we should calculate this based on the size of the device.
 * Only the metadata space map needs this functionality.
 */
#define MAX_RECURSIVE_ALLOCATIONS 32

enum block_op_type {
	BOP_INC,
	BOP_DEC
};

struct block_op {
	enum block_op_type type;
	dm_block_t block;
};

struct sm_disk {
	struct dm_space_map sm;

	struct ll_disk ll;
	struct ll_disk old_ll;

	dm_block_t begin, end;

	unsigned recursion_count;
	unsigned allocated_this_transaction;
	unsigned nr_uncommitted;
	struct block_op uncommitted[MAX_RECURSIVE_ALLOCATIONS];
};

static int add_bop(struct sm_disk *smd, enum block_op_type type, dm_block_t b)
{
	struct block_op *op;

	printk(KERN_ALERT "adding bop %d %u", (int) type, (unsigned) b);
	if (smd->nr_uncommitted == MAX_RECURSIVE_ALLOCATIONS) {
		BUG_ON(1);
		return -1;
	}

	op = smd->uncommitted + smd->nr_uncommitted++;
	op->type = type;
	op->block = b;
	return 0;
}

static int commit_bop(struct sm_disk *smd, struct block_op *op)
{
	int r = 0;

	switch (op->type) {
	case BOP_INC:
		r = ll_inc(&smd->ll, op->block);
		break;

	case BOP_DEC:
		r = ll_dec(&smd->ll, op->block);
		break;
	}

	return r;
}

static void in(struct sm_disk *smd)
{
	smd->recursion_count++;
}

static void out(struct sm_disk *smd)
{
	int r = 0;
	BUG_ON(!smd->recursion_count);

	if (smd->recursion_count == 1 && smd->nr_uncommitted) {
		printk(KERN_ALERT "committing %u bops", (unsigned) smd->nr_uncommitted);
		while (smd->nr_uncommitted && !r)
			r = commit_bop(smd, smd->uncommitted + --smd->nr_uncommitted);
	}

	smd->recursion_count--;
}

static void no_recurse(struct sm_disk *smd)
{
	BUG_ON(smd->recursion_count);
}

static int recursing(struct sm_disk *smd)
{
	return smd->recursion_count;
}

static void sm_disk_destroy(struct dm_space_map *sm)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);
	kfree(smd);
}

static int sm_disk_get_nr_blocks(struct dm_space_map *sm, dm_block_t *count)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);
	*count = smd->ll.nr_blocks;
	return 0;
}

static int sm_disk_get_nr_free(struct dm_space_map *sm, dm_block_t *count)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	*count = smd->old_ll.nr_blocks - smd->old_ll.nr_allocated - smd->allocated_this_transaction;
	return 0;
}

static int sm_disk_get_count(struct dm_space_map *sm, dm_block_t b, uint32_t *result)
{
	int r, i;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);
	unsigned adjustment = 0;

	/*
	 * we may have some uncommitted adjustments to add.  This list
	 * should always be really short.
	 */
	for (i = 0; i < smd->nr_uncommitted; i++) {
		struct block_op *op = smd->uncommitted + i;
		if (op->block == b)
			switch (op->type) {
			case BOP_INC:
				adjustment++;
				break;

			case BOP_DEC:
				adjustment--;
				break;
			}
	}

	r = ll_lookup(&smd->ll, b, result);
	if (r)
		return r;
	*result += adjustment;

	return 0;
}

static int sm_disk_count_is_more_than_one(struct dm_space_map *sm, dm_block_t b, int *result)
{
	int r, i, adjustment = 0;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);
	uint32_t rc;

	/*
	 * we may have some uncommitted adjustments to add.  This list
	 * should always be really short.
	 */
	for (i = 0; i < smd->nr_uncommitted; i++) {
		struct block_op *op = smd->uncommitted + i;
		if (op->block == b)
			switch (op->type) {
			case BOP_INC:
				adjustment++;
				break;

			case BOP_DEC:
				adjustment--;
				break;
			}
	}

	if (adjustment > 1) {
		*result = 1;
		return 0;
	}

	r = ll_lookup_bitmap(&smd->ll, b, &rc);
	if (r)
		return r;

	if (rc == 3)
		/* we err on the side of caution, and always return true */
		*result = 1;
	else
		*result = rc + adjustment > 1;

	return 0;
}

static int sm_disk_set_count(struct dm_space_map *sm, dm_block_t b, uint32_t count)
{
	int r;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	no_recurse(smd);

	in(smd);
	r = ll_insert(&smd->ll, b, count);
	out(smd);
	return r;
}

static int sm_disk_inc_block(struct dm_space_map *sm, dm_block_t b)
{
	int r;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	if (recursing(smd))
		r = add_bop(smd, BOP_INC, b);

	else {
		in(smd);
		r = ll_inc(&smd->ll, b);
		out(smd);
	}
	return r;
}

static int sm_disk_dec_block(struct dm_space_map *sm, dm_block_t b)
{
	int r;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	if (recursing(smd))
		r = add_bop(smd, BOP_DEC, b);

	else {
		in(smd);
		r = ll_dec(&smd->ll, b);
		out(smd);
	}
	return r;
}

static int sm_disk_new_block_(struct dm_space_map *sm, dm_block_t *b)
{
	int r;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	r = ll_find_free_block(&smd->old_ll, smd->begin, smd->end, b);
	if (r)
		return r;

	smd->begin = *b + 1;

	if (recursing(smd))
		r = add_bop(smd, BOP_INC, *b);

	else {
		in(smd);
		r = ll_inc(&smd->ll, *b);
		out(smd);
	}

	if (!r)
		smd->allocated_this_transaction++;
	return r;
}

static int sm_disk_new_block(struct dm_space_map *sm, dm_block_t *b)
{
	int r = sm_disk_new_block_(sm, b);
	if (!r)
		printk(KERN_ALERT "new block returned %u", (unsigned) *b);
	return r;
}

static int sm_disk_commit(struct dm_space_map *sm)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	memcpy(&smd->old_ll, &smd->ll, sizeof(smd->old_ll));
	smd->begin = 0;
	smd->end = smd->ll.nr_blocks;
	smd->allocated_this_transaction = 0;
	return 0;
}

static int sm_disk_root_size(struct dm_space_map *sm, size_t *result)
{
	*result = sizeof(struct sm_root);
	return 0;
}

static int sm_disk_copy_root(struct dm_space_map *sm, void *where, size_t max)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);
	struct sm_root root;

	root.nr_blocks = __cpu_to_le64(smd->ll.nr_blocks);
	root.nr_allocated = __cpu_to_le64(smd->ll.nr_allocated);
	root.bitmap_root = __cpu_to_le64(smd->ll.bitmap_root);
	root.ref_count_root = __cpu_to_le64(smd->ll.ref_count_root);

	if (max < sizeof(root))
		return -ENOSPC;

	memcpy(where, &root, sizeof(root));

	return 0;
}

/*----------------------------------------------------------------*/

/*
 * When a new space map is created, that manages it's own space.  We use
 * this tiny bootstrap allocator.
 */

static void sm_bootstrap_destroy(struct dm_space_map *sm)
{
	BUG_ON(1);
}

static int sm_bootstrap_get_nr_blocks(struct dm_space_map *sm, dm_block_t *count)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);
	return smd->end;
}

static int sm_bootstrap_get_nr_free(struct dm_space_map *sm, dm_block_t *count)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);
	*count = smd->end - smd->begin;
	return 0;
}

static int sm_bootstrap_get_count(struct dm_space_map *sm, dm_block_t b, uint32_t *result)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);
	return b < smd->begin ? 1 : 0;
}

static int sm_bootstrap_count_is_more_than_one(struct dm_space_map *sm, dm_block_t b, int *result)
{
	*result = 0;
	return 0;
}

static int sm_bootstrap_set_count(struct dm_space_map *sm, dm_block_t b, uint32_t count)
{
	BUG_ON(1);
	return -1;
}

static int sm_bootstrap_new_block(struct dm_space_map *sm, dm_block_t *b)
{
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	/*
	 * We know the entire device is unused.
	 */
	if (smd->begin == smd->end)
		return -ENOSPC;

	*b = smd->begin++;
	printk(KERN_ALERT "bootstrap_new_block %u", (unsigned) *b);
	return 0;
}

static int sm_bootstrap_inc_block(struct dm_space_map *sm, dm_block_t b)
{
	BUG_ON(1);
	return -1;
}

static int sm_bootstrap_dec_block(struct dm_space_map *sm, dm_block_t b)
{
	/* FIXME: resolve this */
	printk(KERN_ALERT "leaked %u", (unsigned) b);
	return 0;
}

static int sm_bootstrap_commit(struct dm_space_map *sm)
{
	return 0;
}

static int sm_bootstrap_root_size(struct dm_space_map *sm, size_t *result)
{
	BUG_ON(1);
	return -1;
}

static int sm_bootstrap_copy_root(struct dm_space_map *sm, void *where, size_t max)
{
	BUG_ON(1);
	return -1;
}

/*----------------------------------------------------------------*/

static struct dm_space_map ops_ = {
	.destroy = sm_disk_destroy,
	.get_nr_blocks = sm_disk_get_nr_blocks,
	.get_nr_free = sm_disk_get_nr_free,
	.get_count = sm_disk_get_count,
	.count_is_more_than_one = sm_disk_count_is_more_than_one,
	.set_count = sm_disk_set_count,
	.inc_block = sm_disk_inc_block,
	.dec_block = sm_disk_dec_block,
	.new_block = sm_disk_new_block,
	.commit = sm_disk_commit,
	.root_size = sm_disk_root_size,
	.copy_root = sm_disk_copy_root
};

static struct dm_space_map bootstrap_ops_ = {
	.destroy = sm_bootstrap_destroy,
	.get_nr_blocks = sm_bootstrap_get_nr_blocks,
	.get_nr_free = sm_bootstrap_get_nr_free,
	.get_count = sm_bootstrap_get_count,
	.count_is_more_than_one = sm_bootstrap_count_is_more_than_one,
	.set_count = sm_bootstrap_set_count,
	.inc_block = sm_bootstrap_inc_block,
	.dec_block = sm_bootstrap_dec_block,
	.new_block = sm_bootstrap_new_block,
	.commit = sm_bootstrap_commit,
	.root_size = sm_bootstrap_root_size,
	.copy_root = sm_bootstrap_copy_root
};

struct dm_space_map *dm_sm_disk_init(void)
{
	struct sm_disk *smd;

	smd = kmalloc(sizeof(*smd), GFP_KERNEL);
	if (!smd)
		return ERR_PTR(-ENOMEM);

	memcpy(&smd->sm, &ops_, sizeof(smd->sm));
	return &smd->sm;
}
EXPORT_SYMBOL_GPL(dm_sm_disk_init);

int dm_sm_disk_create(struct dm_space_map *sm,
		      struct dm_transaction_manager *tm,
		      dm_block_t nr_blocks)
{
	int r;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	smd->begin = 0;
	smd->end = nr_blocks;
	smd->recursion_count = 0;
	smd->allocated_this_transaction = 0;
	smd->nr_uncommitted = 0;

	r = ll_new(&smd->ll, tm, nr_blocks);
	if (r)
		return r;

	return sm_disk_commit(sm);
}
EXPORT_SYMBOL_GPL(dm_sm_disk_create);

int dm_sm_disk_create_recursive(struct dm_space_map *sm,
				struct dm_transaction_manager *tm,
				dm_block_t nr_blocks,
				dm_block_t superblock)
{
	int r;
	dm_block_t i;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	smd->begin = superblock + 1;
	smd->end = nr_blocks;
	smd->recursion_count = 0;
	smd->allocated_this_transaction = 0;
	smd->nr_uncommitted = 0;

	memcpy(&smd->sm, &bootstrap_ops_, sizeof(smd->sm));
	r = ll_new(&smd->ll, tm, nr_blocks);
	if (r)
		return r;
	memcpy(&smd->sm, &ops_, sizeof(smd->sm));

	/*
	 * Now we need to update the newly created data structures with the
	 * allocated blocks that they were built from.
	 */
	for (i = 0; !r && i < smd->begin; i++)
		r = ll_inc(&smd->ll, i);

	if (r)
		return r;

	/*
	 * Finally we increment the superblock to reserve it.
	 */
	r = ll_inc(&smd->ll, superblock);
	if (r)
		return r;

	return sm_disk_commit(sm);
}
EXPORT_SYMBOL_GPL(dm_sm_disk_create_recursive);

int dm_sm_disk_open(struct dm_space_map *sm,
		    struct dm_transaction_manager *tm,
		    void *root, size_t len)
{
	int r;
	struct sm_disk *smd = container_of(sm, struct sm_disk, sm);

	r = ll_open(&smd->ll, tm, root, len);
	if (r)
		return r;

	smd->begin = 0;
	smd->end = smd->ll.nr_blocks;
	smd->recursion_count = 0;
	smd->allocated_this_transaction = 0;
	smd->nr_uncommitted = 0;

	return sm_disk_commit(sm);
}
EXPORT_SYMBOL_GPL(dm_sm_disk_open);
