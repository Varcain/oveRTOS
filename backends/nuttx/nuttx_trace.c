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
 * NuttX is a POSIX-shaped kernel, so the trace wiring mirrors posix_*.c
 * instead of the kernel-hook model used by freertos_trace.c. Per-thread
 * state is driven explicitly by:
 *
 *   - thread entry/exit in nuttx_thread.c::task_wrapper()
 *   - sync-primitive wrappers in nuttx_sync.c (BLOCKED before the wait,
 *     RUNNING after), tagged with OVE_TRACE_MARK_CURRENT
 *
 * This file only owns the pieces that don't belong in the thread backend:
 *
 *   - ove_state_stats_now_us()             monotonic time source
 *   - ove_backend_thread_current_handle()  current-task handle for markers
 *   - ove_backend_trace_list_threads()     descriptor enumeration
 */

#include "ove_config.h"

#include <stdint.h>
#include <time.h>

#ifdef CONFIG_OVE_THREAD_STATE_STATS
#include "ove/thread_state_stats.h"

uint64_t ove_state_stats_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL
	     + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

#ifdef CONFIG_OVE_TRACE_STREAM

#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

/* Exported from nuttx_thread.c so this file doesn't duplicate the
 * task-TLS index plumbing. */
extern struct ove_thread *ove_nuttx_current_thread(void);

/* Thread registry lives in nuttx_thread.c. Descriptor enumeration
 * walks it directly rather than nxsched_foreach, so the swimlane
 * only shows threads that can actually emit state records — kernel
 * idle/workq tasks never emit, so listing them would create empty rows. */
extern struct ove_thread *ove_nuttx_thread_list_head;
extern void ove_nuttx_thread_list_lock(void);
extern void ove_nuttx_thread_list_unlock(void);

uintptr_t ove_backend_thread_current_handle(void)
{
	return (uintptr_t)ove_nuttx_current_thread();
}

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out,
				      size_t max)
{
	if (!out || max == 0)
		return 0;

	size_t count = 0;
	ove_nuttx_thread_list_lock();
	for (struct ove_thread *t = ove_nuttx_thread_list_head;
	     t && count < max; t = t->next) {
		out[count].tid  = (uint32_t)(uintptr_t)t;
		out[count].name = t->name ? t->name : "?";
		count++;
	}
	ove_nuttx_thread_list_unlock();
	return count;
}

#endif /* CONFIG_OVE_TRACE_STREAM */
