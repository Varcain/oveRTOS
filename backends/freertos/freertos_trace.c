/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * FreeRTOS trace backend.
 *
 * POSIX/WASM own the thread state and call ove_state_track_transition
 * directly at each state change. FreeRTOS owns state inside the kernel,
 * so we hook the kernel's trace macros:
 *
 *   traceTASK_SWITCHED_OUT()       — running task is being switched off CPU
 *   traceTASK_SWITCHED_IN()        — selected task is taking CPU
 *   traceBLOCKING_ON_*(obj)        — running task is about to block on a primitive
 *   traceTASK_DELAY / DELAY_UNTIL  — running task is about to sleep
 *
 * traceTASK_SWITCHED_{IN,OUT} fire from inside vTaskSwitchContext() in the
 * PendSV handler — API calls that walk kernel lists (eTaskGetState, etc.)
 * are not safe there. Only application task tag reads and plain memory
 * accesses are. Blocking state is therefore set earlier from the
 * traceBLOCKING_ON_* / traceTASK_DELAY macros, which fire in task context.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

#include "FreeRTOS.h"
#include "task.h"

#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h"
#include "ove/types.h"

/* ── Time source (microsecond resolution via SysTick) ────────────── */

/* SysTick registers — CMSIS layout, always mapped on Cortex-M. */
#define TRACE_SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014)
#define TRACE_SYSTICK_VAL  (*(volatile uint32_t *)0xE000E018)

extern uint32_t SystemCoreClock;

uint64_t ove_state_stats_now_us(void)
{
	uint32_t load = TRACE_SYSTICK_LOAD;
	TickType_t t1, t2;
	uint32_t val;

	if (xPortIsInsideInterrupt()) {
		/* SysTick VAL can wrap across the tick boundary; read it
		 * between two tick reads so we can retry on mismatch. The
		 * ISR variant reads xTickCount directly and is safe from
		 * PendSV / SysTick handlers. */
		t1  = xTaskGetTickCountFromISR();
		val = TRACE_SYSTICK_VAL;
		t2  = xTaskGetTickCountFromISR();
	} else {
		do {
			t1  = xTaskGetTickCount();
			val = TRACE_SYSTICK_VAL;
			t2  = xTaskGetTickCount();
		} while (t1 != t2);
	}

	uint32_t elapsed_cycles = (load + 1) - val;
	uint64_t us_frac = (uint64_t)elapsed_cycles * 1000000ULL
	                 / (uint64_t)SystemCoreClock;
	return (uint64_t)t1 * (1000000ULL / configTICK_RATE_HZ) + us_frac;
}

/* ── Current-thread handle for OVE_TRACE_MARK_CURRENT ─────────────── */

uintptr_t ove_backend_thread_current_handle(void)
{
	TaskHandle_t h = xTaskGetCurrentTaskHandle();
	if (!h)
		return 0;
	return (uintptr_t)xTaskGetApplicationTaskTag(h);
}

/* ── Kernel trace hooks ───────────────────────────────────────────── */

/*
 * Emit RUNNING -> (state) for the outgoing task. Called from
 * traceTASK_SWITCHED_OUT(). We don't know the new state authoritatively
 * here (kernel list walk is not safe), so we infer:
 *   - if the tracker already says BLOCKED/SUSPENDED, keep it
 *     (traceBLOCKING_ON_* or traceTASK_DELAY already flipped us)
 *   - otherwise READY (pre-emption or yield)
 */
void ove_backend_trace_task_switched_out(void)
{
	TaskHandle_t h = xTaskGetCurrentTaskHandle();
	if (!h)
		return;
	struct ove_thread *t = (struct ove_thread *)xTaskGetApplicationTaskTag(h);
	if (!t)
		return;  /* idle / timer / tasks not created via ove_thread_init */

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	int prev = t->st.cur_state;
	int next;
	if (prev == OVE_THREAD_STATE_BLOCKED ||
	    prev == OVE_THREAD_STATE_SUSPENDED)
		next = prev;
	else
		next = OVE_THREAD_STATE_READY;

	if (prev != next)
		ove_trace_emit_state((uintptr_t)t, prev, next);
	ove_state_track_transition(&t->st, next);
#endif
}

/*
 * Emit previous_state -> RUNNING for the incoming task. Called from
 * traceTASK_SWITCHED_IN().
 */
void ove_backend_trace_task_switched_in(void)
{
	TaskHandle_t h = xTaskGetCurrentTaskHandle();
	if (!h)
		return;
	struct ove_thread *t = (struct ove_thread *)xTaskGetApplicationTaskTag(h);
	if (!t)
		return;

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	int prev = t->st.cur_state;
	ove_trace_emit_state((uintptr_t)t, prev, OVE_THREAD_STATE_RUNNING);
	ove_state_track_transition(&t->st, OVE_THREAD_STATE_RUNNING);
#endif
}

/*
 * Called from traceBLOCKING_ON_* / traceTASK_DELAY / traceTASK_DELAY_UNTIL.
 * These fire in task context (before the yield), so list-walking FreeRTOS
 * API would be safe — but we don't need it, we just flip the tracker to
 * BLOCKED so the subsequent SWITCHED_OUT hook preserves it.
 */
void ove_backend_trace_task_blocking(void)
{
	TaskHandle_t h = xTaskGetCurrentTaskHandle();
	if (!h)
		return;
	struct ove_thread *t = (struct ove_thread *)xTaskGetApplicationTaskTag(h);
	if (!t)
		return;

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	int prev = t->st.cur_state;
	if (prev != OVE_THREAD_STATE_BLOCKED)
		ove_trace_emit_state((uintptr_t)t, prev,
				     OVE_THREAD_STATE_BLOCKED);
	ove_state_track_transition(&t->st, OVE_THREAD_STATE_BLOCKED);
#endif
}

/* ── Descriptor enumeration for sim_trace plugin ─────────────────── */

#include "ove/sim/ove_sim_trace.h"
#include "ove_trace_ring.h"

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out,
				      size_t max)
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
	UBaseType_t filled = uxTaskGetSystemState(tasks, count, NULL);

	size_t n = 0;
	for (UBaseType_t i = 0; i < filled && n < max; i++) {
		struct ove_thread *t = (struct ove_thread *)
			xTaskGetApplicationTaskTag(tasks[i].xHandle);
		/* Tasks without an ove_thread wrapper (idle, timer) are
		 * still emitted so the swimlane shows their rows; the tid
		 * falls back to the raw FreeRTOS TCB address. */
		uintptr_t handle = t ? (uintptr_t)t
				     : (uintptr_t)tasks[i].xHandle;
		out[n].tid  = (uint32_t)handle;
		out[n].name = tasks[i].pcTaskName;
		n++;
	}
	return n;
#else
	(void)out;
	(void)max;
	return 0;
#endif
}

#endif /* CONFIG_OVE_TRACE_STREAM */
