/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Zephyr trace backend.
 *
 * Zephyr's CONFIG_TRACING_USER hooks would give us kernel-driven state
 * transitions, but the swimlane only cares about oveRTOS-managed threads
 * and the transitions the public API already performs at
 * ove_thread_sleep_ms / suspend / resume / sync-primitive wrappers. So
 * this backend mirrors posix_*.c and nuttx_*.c — state is driven
 * explicitly from zephyr_thread.c::thread_wrapper() and the sync wrappers
 * in zephyr_sync.c.
 *
 * This file only owns the pieces that don't belong in the thread backend:
 *
 *   - ove_state_stats_now_us()             monotonic time source
 *   - ove_backend_thread_current_handle()  current-task handle for markers
 *   - ove_backend_trace_list_threads()     descriptor enumeration
 */

#include "ove_config.h"

#include <stdint.h>

#include <zephyr/kernel.h>

#ifdef CONFIG_OVE_THREAD_STATE_STATS
#include "ove/thread_state_stats.h"

uint64_t ove_state_stats_now_us(void)
{
	/* k_uptime_ticks + k_ticks_to_us_near64 is portable across all
	 * Zephyr arches (cycle_get_64 isn't guaranteed without
	 * CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) and gives tick-resolution
	 * monotonic microseconds. Safe to call from any context — both
	 * helpers are pure arithmetic / atomic tick reads. */
	return k_ticks_to_us_near64(k_uptime_ticks());
}
#endif

#ifdef CONFIG_OVE_TRACE_STREAM

#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

/* Exported from zephyr_thread.c so this file doesn't duplicate the
 * k_thread_custom_data plumbing. Returns NULL for the idle thread and
 * any non-oveRTOS system threads (whose custom_data slot is never set). */
extern struct ove_thread *ove_zephyr_current_thread(void);

/* Thread registry lives in zephyr_thread.c. Descriptor enumeration
 * walks it directly rather than k_thread_foreach_unlocked, so the
 * swimlane only shows threads that can actually emit state records —
 * Zephyr idle/system threads never emit, so listing them would create
 * empty rows. */
extern struct ove_thread *ove_zephyr_thread_list_head;
extern void ove_zephyr_thread_list_lock(void);
extern void ove_zephyr_thread_list_unlock(void);

uintptr_t ove_backend_thread_current_handle(void)
{
	return (uintptr_t)ove_zephyr_current_thread();
}

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max)
{
	if (!out || max == 0)
		return 0;

	size_t count = 0;
	ove_zephyr_thread_list_lock();
	for (struct ove_thread *t = ove_zephyr_thread_list_head; t && count < max; t = t->next) {
		out[count].tid = (uint32_t)(uintptr_t)t;
		out[count].name = t->name ? t->name : "?";
		count++;
	}
	ove_zephyr_thread_list_unlock();
	return count;
}

#endif /* CONFIG_OVE_TRACE_STREAM */
