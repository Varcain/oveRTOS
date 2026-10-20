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
 * Thread state transitions come from the kernel's CONFIG_TRACING_USER
 * scheduler hooks in zephyr_thread.c; the sync wrappers in zephyr_sync.c
 * add sync-primitive markers.
 *
 * Thread identity (shared with the profiler): the ove_thread for threads
 * created through ove_thread_*, otherwise the k_thread itself; 0 for
 * interrupt context.
 *
 * This file owns:
 *
 *   - ove_backend_thread_current_handle()  current-thread tid for markers
 *   - ove_backend_trace_list_threads()     tid -> name for every kernel
 *                                          thread (CONFIG_THREAD_MONITOR)
 *
 * The time source lives in zephyr_state_stats.c.
 */

#include "ove_config.h"

#include <stdint.h>

#ifdef CONFIG_OVE_TRACE_STREAM

#include <zephyr/kernel.h>

#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

static uintptr_t thread_tid(const struct k_thread *thread)
{
	const void *ove = thread->custom_data;
	return ove ? (uintptr_t)ove : (uintptr_t)thread;
}

uintptr_t ove_backend_thread_current_handle(void)
{
	if (k_is_in_isr())
		return 0;
	return thread_tid(k_current_get());
}

struct list_ctx {
	struct ove_trace_thread_desc *out;
	size_t max;
	size_t count;
};

static void list_one(const struct k_thread *thread, void *user_data)
{
	struct list_ctx *c = user_data;
	if (c->count >= c->max)
		return;
	const struct ove_thread *ove = thread->custom_data;
	const char *name = ove && ove->name ? ove->name : k_thread_name_get((k_tid_t)thread);
	c->out[c->count].tid = (uint32_t)thread_tid(thread);
	c->out[c->count].generation = ove ? ove->generation : 0u;
	c->out[c->count].name = name && name[0] ? name : "?";
	c->count++;
}

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max)
{
	if (!out || max == 0)
		return 0;
	struct list_ctx c = {.out = out, .max = max, .count = 0};
	/* Names point into the k_thread / ove_thread objects; the caller
	 * serialises them right away from the sim-debug pump. */
	k_thread_foreach(list_one, &c);
	return c.count;
}

#endif /* CONFIG_OVE_TRACE_STREAM */
