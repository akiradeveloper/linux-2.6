#include "btree-internal.h"

/*----------------------------------------------------------------*/

int bn_read_lock(struct btree_info *info, block_t b, struct block **result)
{
	return tm_read_lock(info->tm, b, result);
}

int bn_shadow(struct btree_info *info, block_t orig, count_adjust_fn fn,
	      struct block **result, int *inc)
{
	int r;

	r = tm_shadow_block(info->tm, orig, result, inc);
	if (r == 0 && *inc)
		inc_children(info, to_node(*result), fn);

	return r;
}

int bn_new_block(struct btree_info *info, struct block **result)
{
	return tm_new_block(info->tm, result);
}

int bn_unlock(struct btree_info *info, struct block *b)
{
	return tm_unlock(info->tm, b);
}

/*----------------------------------------------------------------*/

void init_ro_spine(struct ro_spine *s, struct btree_info *info)
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

int ro_step(struct ro_spine *s, block_t new_child)
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
	struct block *n;
	BUG_ON(!s->count);
	n = s->nodes[s->count - 1];
	return to_node(n);
}

/*----------------------------------------------------------------*/

void init_shadow_spine(struct shadow_spine *s, struct btree_info *info)
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

int shadow_step(struct shadow_spine *s, block_t b, count_adjust_fn fn, int *inc)
{
	int r;

	if (s->count == 2) {
		r = bn_unlock(s->info, s->nodes[0]);
		if (r < 0)
			return r;
		s->nodes[0] = s->nodes[1];
		s->count--;
	}

	r = bn_shadow(s->info, b, fn, s->nodes + s->count, inc);
	if (r == 0) {
		if (s->count == 0)
			s->root = block_location(s->nodes[0]);

		s->count++;
	}

	return r;
}

struct block *shadow_current(struct shadow_spine *s)
{
	return s->nodes[s->count - 1];
}

struct block *shadow_parent(struct shadow_spine *s)
{
	return s->count == 2 ? s->nodes[0] : NULL;
}

int shadow_root(struct shadow_spine *s)
{
	return s->root;
}

/*----------------------------------------------------------------*/
