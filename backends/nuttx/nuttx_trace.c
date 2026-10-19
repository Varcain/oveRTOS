/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * NuttX trace backend.
 *
 * Thread state transitions come from the scheduler, through the sched_note
 * driver in nuttx_thread.c (suspend/resume on every context switch); the
 * sync wrappers in nuttx_sync.c add sync-primitive markers.
 *
 * Thread identity (shared with the profiler): the ove_thread for tasks
 * created through ove_thread_*, otherwise the task's TCB; 0 for interrupt
 * context.
 *
 * This file owns:
 *
 *   - ove_backend_thread_current_handle()  current-task tid for markers
 *   - ove_backend_trace_list_threads()     tid -> name for every task
 *
 * The time source lives in nuttx_state_stats.c.
 */

#include "ove_config.h"

#include <stdint.h>

#ifdef CONFIG_OVE_TRACE_STREAM

#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/sched.h>

#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

/* Exported from nuttx_thread.c. */
extern struct ove_thread *ove_nuttx_current_thread(void);
extern struct ove_thread *ove_nuttx_thread_by_pid(pid_t pid);

uintptr_t ove_backend_thread_current_handle(void)
{
	if (up_interrupt_context())
		return 0;
	struct ove_thread *t = ove_nuttx_current_thread();
	return t ? (uintptr_t)t : (uintptr_t)nxsched_self();
}

#define DESC_NAME_MAX 32

struct list_ctx {
	struct ove_trace_thread_desc *out;
	size_t max;
	size_t count;
};

/* Names are copied: a TCB can be freed as soon as the enumeration's
 * critical section is released. Single caller (the sim-debug pump). */
static char desc_names[32][DESC_NAME_MAX];

static void list_one(FAR struct tcb_s *tcb, FAR void *arg)
{
	struct list_ctx *c = arg;
	if (c->count >= c->max || c->count >= sizeof(desc_names) / sizeof(desc_names[0]))
		return;
	struct ove_thread *t = ove_nuttx_thread_by_pid(tcb->pid);
	const char *name = (t && t->name) ? t->name : NULL;
#if CONFIG_TASK_NAME_SIZE > 0
	if (!name)
		name = tcb->name;
#endif
	char *dst = desc_names[c->count];
	strlcpy(dst, (name && name[0]) ? name : "?", DESC_NAME_MAX);
	c->out[c->count].tid = (uint32_t)(t ? (uintptr_t)t : (uintptr_t)tcb);
	c->out[c->count].name = dst;
	c->count++;
}

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max)
{
	if (!out || max == 0)
		return 0;
	struct list_ctx c = {.out = out, .max = max, .count = 0};
	nxsched_foreach(list_one, &c);
	return c.count;
}

#endif /* CONFIG_OVE_TRACE_STREAM */
