#include "dm-btree-internal.h"

/*----------------------------------------------------------------*/

static void node_prepare_for_write(struct dm_block_validator *v,
				   struct dm_block *b,
				   size_t block_size)
{
	struct node_header *node = dm_block_data(b);

	node->blocknr = __cpu_to_le64(dm_block_location(b));
	node->csum = dm_block_csum_data(&node->flags,
					block_size - sizeof(u32));
}

static int node_check(struct dm_block_validator *v,
		      struct dm_block *b,
		      size_t block_size)
{
	struct node_header *node = dm_block_data(b);
	__le32 csum;

	if (dm_block_location(b) != __le64_to_cpu(node->blocknr)) {
		printk(KERN_ERR "btree node_check failed blocknr %llu "
		       "wanted %llu\n", __le64_to_cpu(node->blocknr), dm_block_location(b));
		return -ENOTBLK;
	}

	csum = dm_block_csum_data(&node->flags,
				  block_size - sizeof(u32));
	if (csum != node->csum) {
		printk(KERN_ERR "btree node_check failed csum %u wanted %u\n",
		       __le32_to_cpu(csum), __le32_to_cpu(node->csum));
		return -EILSEQ;
	}

	return 0;
}

struct dm_block_validator btree_node_validator = {
	.name = "btree_node",
	.prepare_for_write = node_prepare_for_write,
	.check = node_check
};

/*----------------------------------------------------------------*/

int bn_read_lock(struct dm_btree_info *info, dm_block_t b,
		 struct dm_block **result)
{
	return dm_tm_read_lock(info->tm, b, &btree_node_validator, result);
}

int bn_shadow(struct dm_btree_info *info, dm_block_t orig,
	      struct dm_btree_value_type *vt,
	      struct dm_block **result, int *inc)
{
	int r;

	r = dm_tm_shadow_block(info->tm, orig, &btree_node_validator, result, inc);
	if (r == 0 && *inc)
		inc_children(info->tm, dm_block_data(*result), vt);

	return r;
}

int bn_new_block(struct dm_btree_info *info, struct dm_block **result)
{
	return dm_tm_new_block(info->tm, &btree_node_validator, result);
}

int bn_unlock(struct dm_btree_info *info, struct dm_block *b)
{
	return dm_tm_unlock(info->tm, b);
}

/*----------------------------------------------------------------*/

void init_ro_spine(struct ro_spine *s, struct dm_btree_info *info)
{
	s->info = info;
	s->count = 0;
	s->nodes[0] = NULL;
	s->nodes[1] = NULL;
}

int exit_ro_spine(struct ro_spine *s)
{
	int r = 0, i;

	for (i = 0; i < s->count; i++) {
		int r2 = bn_unlock(s->info, s->nodes[i]);
		if (r2 < 0)
			r = r2;
	}

	return r;
}

int ro_step(struct ro_spine *s, dm_block_t new_child)
{
	int r;

	if (s->count == 2) {
		r = bn_unlock(s->info, s->nodes[0]);
		if (r < 0)
			return r;
		s->nodes[0] = s->nodes[1];
		s->count--;
	}

	r = bn_read_lock(s->info, new_child, s->nodes + s->count);
	if (r == 0)
		s->count++;

	return r;
}

struct node *ro_node(struct ro_spine *s)
{
	struct dm_block *n;
	BUG_ON(!s->count);
	n = s->nodes[s->count - 1];
	return dm_block_data(n);
}

/*----------------------------------------------------------------*/

void init_shadow_spine(struct shadow_spine *s, struct dm_btree_info *info)
{
	s->info = info;
	s->count = 0;
}

int exit_shadow_spine(struct shadow_spine *s)
{
	int r = 0, i;

	for (i = 0; i < s->count; i++) {
		int r2 = bn_unlock(s->info, s->nodes[i]);
		if (r2 < 0)
			r = r2;
	}

	return r;
}

int shadow_step(struct shadow_spine *s, dm_block_t b,
		struct dm_btree_value_type *vt, int *inc)
{
	int r;

	if (s->count == 2) {
		r = bn_unlock(s->info, s->nodes[0]);
		if (r < 0)
			return r;
		s->nodes[0] = s->nodes[1];
		s->count--;
	}

	r = bn_shadow(s->info, b, vt, s->nodes + s->count, inc);
	if (r == 0) {
		if (s->count == 0)
			s->root = dm_block_location(s->nodes[0]);

		s->count++;
	}

	return r;
}

struct dm_block *shadow_current(struct shadow_spine *s)
{
	return s->nodes[s->count - 1];
}

struct dm_block *shadow_parent(struct shadow_spine *s)
{
	return s->count == 2 ? s->nodes[0] : NULL;
}

int shadow_root(struct shadow_spine *s)
{
	return s->root;
}

/*----------------------------------------------------------------*/
