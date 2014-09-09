/*
 * Copyright (C) 2012-2014 Akira Hayakawa <ruby.wktk@gmail.com>
 *
 * This file is released under the GPL.
 */

#ifndef DM_WRITEBOOST_DAEMON_H
#define DM_WRITEBOOST_DAEMON_H

/*----------------------------------------------------------------*/

void flush_proc(struct work_struct *);
void wait_for_flushing(struct wb_device *, u64 id);

/*----------------------------------------------------------------*/

void queue_barrier_io(struct wb_device *, struct bio *);
void flush_barrier_ios(struct work_struct *);

/*----------------------------------------------------------------*/

int writeback_proc(void *);
void wait_for_writeback(struct wb_device *, u64 id);

/*----------------------------------------------------------------*/

int modulator_proc(void *);

/*----------------------------------------------------------------*/

int sync_proc(void *);

/*----------------------------------------------------------------*/

int recorder_proc(void *);

/*----------------------------------------------------------------*/

#endif
