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
 * State transitions are emitted by the kernel trace hooks in
 * freertos_state_stats.c. This file only owns the pieces specific to the
 * trace stream:
 *
 *   - ove_backend_thread_current_handle()  current-task handle for markers
 *   - ove_backend_trace_list_threads()     tid -> name descriptors
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

#include "FreeRTOS.h"
#include "task.h"

#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/trace.h"
#include "ove/types.h"

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

/* ── Descriptor enumeration for sim_trace plugin ─────────────────── */

#include "ove/sim/ove_sim_trace.h"
#include "ove_trace_ring.h"

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max)
{
#if configUSE_TRACE_FACILITY
	if (!out || max == 0)
		return 0;

	UBaseType_t count = uxTaskGetNumberOfTasks();
	if (count > (UBaseType_t)max)
		count = (UBaseType_t)max;

	/* uxTaskGetSystemState scratch buffer. Cap at OVE_SIM_TRACE_MAX_DESC
	 * (32) since that's the per-emission cap anyway; a VLA sized off a
	 * runtime count gets ugly on stack-tight targets. */
	TaskStatus_t tasks[OVE_SIM_TRACE_MAX_DESC];
	if (count > OVE_SIM_TRACE_MAX_DESC)
		count = OVE_SIM_TRACE_MAX_DESC;
	/* Keep the scheduler suspended while the tags are read: a thread
	 * destroyed in between would free the wrapper they point to. */
	vTaskSuspendAll();
	UBaseType_t filled = uxTaskGetSystemState(tasks, count, NULL);

	size_t n = 0;
	for (UBaseType_t i = 0; i < filled && n < max; i++) {
		struct ove_thread *t =
			(struct ove_thread *)xTaskGetApplicationTaskTag(tasks[i].xHandle);
		/* Tasks without an ove_thread wrapper (idle, timer) are
		 * still emitted so the swimlane shows their rows; the tid
		 * falls back to the raw FreeRTOS TCB address. */
		uintptr_t handle = t ? (uintptr_t)t : (uintptr_t)tasks[i].xHandle;
		out[n].tid = (uint32_t)handle;
		out[n].generation = t ? t->generation : 0u;
		out[n].name = tasks[i].pcTaskName;
		n++;
	}
	(void)xTaskResumeAll();
	return n;
#else
	(void)out;
	(void)max;
	return 0;
#endif
}

#endif /* CONFIG_OVE_TRACE_STREAM */
