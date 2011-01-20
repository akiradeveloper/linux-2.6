#ifndef SNAPSHOTS_BTREE_INTERNAL_H
#define SNAPSHOTS_BTREE_INTERNAL_H

#include "btree.h"

#include <linux/list.h>

/*----------------------------------------------------------------*/

/* FIXME: move all this into btree.c */

enum node_flags {
        INTERNAL_NODE = 1,
        LEAF_NODE = 1 << 1
};

/*
 * To ease coding I'm packing all the different node types into one
 * structure.  We can optimise later.
 */
struct node_header {
        __le32 flags;
        __le32 nr_entries;
	__le32 max_entries;
	__le32 magic;
};

#define BTREE_NODE_MAGIC 160774

struct node {
	struct node_header header;
	__le64 keys[0];
};


/*
 * Based on the ideas in ["B-trees, Shadowing, and Clones" Ohad Rodeh]
 */

/* FIXME: enable close packing for on disk structures */


void inc_children(struct btree_info *info, struct node *n, count_adjust_fn fn);

static inline struct node *to_node(struct block *b)
{
	/* FIXME: this function should fail, rather than fall over */
	struct node *n = (struct node *) block_data(b);
	BUG_ON(__le32_to_cpu(n->header.magic) != BTREE_NODE_MAGIC);
	return n;
}

// FIXME: I don't like the bn_ prefix for these, refers to an old struct block_node
int bn_read_lock(struct btree_info *info, block_t b, struct block **result);
int bn_shadow(struct btree_info *info, block_t orig, count_adjust_fn fn,
	      struct block **result, int *inc);
int bn_new_block(struct btree_info *info, struct block **result);
int bn_unlock(struct btree_info *info, struct block *b);

/*
 * Spines keep track of the rolling locks.  There are 2 variants, read-only
 * and one that uses shadowing.  These are separate structs to allow the
 * type checker to spot misuse, for example accidentally calling read_lock
 * on a shadow spine.
 */
struct ro_spine {
	struct btree_info *info;

	int count;
	struct block *nodes[2];
};

void init_ro_spine(struct ro_spine *s, struct btree_info *info);
int exit_ro_spine(struct ro_spine *s);
int ro_step(struct ro_spine *s, block_t new_child);
struct node *ro_node(struct ro_spine *s);

struct shadow_spine {
	struct btree_info *info;

	int count;
	struct block *nodes[2];

	block_t root;
};

void init_shadow_spine(struct shadow_spine *s, struct btree_info *info);
int exit_shadow_spine(struct shadow_spine *s);
int shadow_step(struct shadow_spine *s, block_t b, count_adjust_fn fn, int *inc);
struct block *shadow_current(struct shadow_spine *s);
struct block *shadow_parent(struct shadow_spine *s);
int shadow_root(struct shadow_spine *s);

/*
 * Some inlines.
 */
static inline uint64_t *key_ptr(struct node *n, uint32_t index)
{
	return n->keys + index;
}

static inline void *value_base(struct node *n)
{
	return &n->keys[__le32_to_cpu(n->header.max_entries)];
}

static inline void *value_ptr(struct node *n, uint32_t index, size_t value_size)
{
	return value_base(n) + (value_size * index);
}

/* assumes the values are suitably aligned and converts to core format */
static inline uint64_t value64(struct node *n, uint32_t index)
{
	__le64 *values = value_base(n);
	return __le64_to_cpu(values[index]);
}

/*
 * Exported for testing.
 */
uint32_t calc_max_entries(size_t value_size, size_t block_size);
void insert_at(size_t value_size,
	       struct node *node, unsigned index, uint64_t key, void *value);
int btree_merge(struct shadow_spine *s, unsigned parent_index, size_t value_size);

/*----------------------------------------------------------------*/

#endif
