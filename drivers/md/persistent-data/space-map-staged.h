#ifndef SNAPSHOTS_SPACE_MAP_STAGED_H
#define SNAPSHOTS_SPACE_MAP_STAGED_H

#include "space-map.h"

/*----------------------------------------------------------------*/

/*
 * Space map disk is not reentrant.  If it's being used to track space for
 * the same device it's placed on it needs to be.  A staged space map
 * breaks this recursion, instead updating in batches.
 */
struct space_map *sm_staged_create(struct space_map *wrappee);

/*
 * If you're creating a new space map you'll need to start by wrapping an
 * in core map, and then swap in the newly created sm_disk.
 * FIXME: we don't need to do this in the kernel.
 */
int sm_staged_set_wrappee(struct space_map *sm,
			  struct space_map *wrappee);

/*
 * A dummy space map only for use before the first staged commit.
 * (ie. while you set up a disk sm).
 */
struct space_map *sm_dummy_create(block_t nr_blocks);

/*----------------------------------------------------------------*/

#endif
