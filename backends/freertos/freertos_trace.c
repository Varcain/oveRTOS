/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * FreeRTOS trace backend (CONFIG_OVE_TRACE_STREAM).
 *
 * This file provides the current-task identity for trace markers.
 * freertos_state_stats.c emits state transitions from the kernel trace hooks.
 * freertos_thread.c supplies thread descriptors from its task registry.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

#include "FreeRTOS.h"
#include "task.h"

#include "ove/trace.h"

/* ── Current-thread handle for OVE_TRACE_MARK_CURRENT ─────────────── */

/* Thread identity (shared with the profiler and the descriptors): the
 * ove_thread for tasks created through ove_thread_*, otherwise the TCB;
 * 0 for interrupt context. */
uintptr_t ove_backend_thread_current_handle(void)
{
	if (xPortIsInsideInterrupt())
		return 0;
	TaskHandle_t h = xTaskGetCurrentTaskHandle();
	if (!h)
		return 0;
	uintptr_t tag = (uintptr_t)xTaskGetApplicationTaskTag(h);
	return tag ? tag : (uintptr_t)h;
}

#endif /* CONFIG_OVE_TRACE_STREAM */
