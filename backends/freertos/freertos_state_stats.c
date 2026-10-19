/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * FreeRTOS per-thread state tracking (CONFIG_OVE_THREAD_STATE_STATS).
 *
 * FreeRTOS owns thread state inside the kernel, so the tracker is driven
 * from the kernel's trace macros (wired in FreeRTOSConfig.h.j2):
 *
 *   traceTASK_SWITCHED_OUT()       — running task is being switched off CPU
 *   traceTASK_SWITCHED_IN()        — selected task is taking CPU
 *   traceBLOCKING_ON_QUEUE_*(obj),
 *   traceEVENT_GROUP_{WAIT_BITS,SYNC}_BLOCK,
 *   traceTASK_NOTIFY_{TAKE,WAIT}_BLOCK,
 *   traceTASK_DELAY / DELAY_UNTIL  — running task is about to block or sleep
 *   traceTASK_SUSPEND(tcb)         — any task is being suspended
 *   traceMOVED_TASK_TO_READY_STATE — a task enters a ready list (wake-up)
 *
 * traceTASK_SWITCHED_{IN,OUT} fire from inside vTaskSwitchContext() in the
 * PendSV handler — API calls that walk kernel lists (eTaskGetState, etc.)
 * are not safe there. Only application task tag reads and plain memory
 * accesses are. Blocking state is therefore set earlier from the
 * blocking macros, which fire in task context; SWITCHED_OUT keeps it.
 *
 * Each transition is also emitted to the trace stream; with
 * CONFIG_OVE_TRACE_STREAM off, ove_trace_emit_state() is an empty inline.
 *
 * Also provides the time source shared by the tracker, the trace stream
 * and the sampling profiler (which depends on this option).
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_THREAD_STATE_STATS

#include "FreeRTOS.h"
#include "task.h"

#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h"
#include "ove/types.h"
#include "ove_freertos_systick.h"

/* ── Time source (microsecond resolution via SysTick) ────────────── */

extern uint32_t SystemCoreClock;

static uint64_t now_us_last;

uint64_t ove_state_stats_now_us(void)
{
	TickType_t ticks;
	uint32_t elapsed;

	/* Pending-reload-safe snapshot: also correct from PendSV, ISRs and
	 * critical sections (see ove_freertos_systick.h). SysTick is clocked
	 * from the core clock (the port's default configSYSTICK_CLOCK_HZ).
	 * Snapshot and clamp share one masked region. */
	UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
	ove_freertos_systick_read(&ticks, &elapsed);

	uint64_t us = (uint64_t)ticks * 1000000ULL / configTICK_RATE_HZ +
		      (uint64_t)elapsed * 1000000ULL / SystemCoreClock;
	us = ove_freertos_monotonic(&now_us_last, us,
				    OVE_FREERTOS_MONOTONIC_HOLD_TICKS * 1000000ULL /
					    configTICK_RATE_HZ);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
	return us;
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
	/* PendSV context: the task-level getter would enter a critical section. */
	struct ove_thread *t = (struct ove_thread *)xTaskGetApplicationTaskTagFromISR(h);
	if (!t)
		return; /* idle / timer / tasks not created via ove_thread_init */

	int prev = t->st.cur_state;
	int next;
	if (prev == OVE_THREAD_STATE_BLOCKED || prev == OVE_THREAD_STATE_SUSPENDED)
		next = prev;
	else
		next = OVE_THREAD_STATE_READY;

	if (prev != next)
		ove_trace_emit_state((uintptr_t)t, prev, next);
	ove_state_track_transition(&t->st, next);
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
	struct ove_thread *t = (struct ove_thread *)xTaskGetApplicationTaskTagFromISR(h);
	if (!t)
		return;

	int prev = t->st.cur_state;
	ove_trace_emit_state((uintptr_t)t, prev, OVE_THREAD_STATE_RUNNING);
	ove_state_track_transition(&t->st, OVE_THREAD_STATE_RUNNING);
}

/*
 * Called from the blocking macros (queue / event group / task
 * notification waits, delays; stream buffers block in a notification
 * wait). These fire in task context (before
 * the yield), so list-walking FreeRTOS
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

	int prev = t->st.cur_state;
	if (prev != OVE_THREAD_STATE_BLOCKED)
		ove_trace_emit_state((uintptr_t)t, prev, OVE_THREAD_STATE_BLOCKED);
	ove_state_track_transition(&t->st, OVE_THREAD_STATE_BLOCKED);
}

/*
 * Called from traceTASK_SUSPEND(pxTCB) inside vTaskSuspend()'s critical
 * section, for any task (not only the caller). @task_tag is the task's
 * application tag, i.e. its ove_thread wrapper (0 for kernel tasks).
 * Plain memory accesses only.
 */
void ove_backend_trace_task_suspended(uintptr_t task_tag)
{
	struct ove_thread *t = (struct ove_thread *)task_tag;
	if (!t)
		return;

	int prev = t->st.cur_state;
	if (prev == OVE_THREAD_STATE_SUSPENDED)
		return;
	ove_trace_emit_state((uintptr_t)t, prev, OVE_THREAD_STATE_SUSPENDED);
	ove_state_track_transition(&t->st, OVE_THREAD_STATE_SUSPENDED);
}

/*
 * Called from traceMOVED_TASK_TO_READY_STATE(pxTCB) — prvAddTaskToReadyList()
 * — whenever a task is (re)inserted into a ready list: wake-up from a
 * blocking wait or delay, resume, or creation. Runs in ISR and
 * critical-section context, so plain memory accesses only.
 *
 * prvAddTaskToReadyList() also re-inserts tasks that are already ready or
 * running (priority change, priority inheritance), which are not wake-ups;
 * only a BLOCKED or SUSPENDED task becomes READY here. The swimlane then
 * shows the wake-to-run latency as READY rather than folding it into
 * BLOCKED.
 */
void ove_backend_trace_task_ready(uintptr_t task_tag)
{
	struct ove_thread *t = (struct ove_thread *)task_tag;
	if (!t)
		return;

	int prev = t->st.cur_state;
	if (prev != OVE_THREAD_STATE_BLOCKED && prev != OVE_THREAD_STATE_SUSPENDED)
		return;
	ove_trace_emit_state((uintptr_t)t, prev, OVE_THREAD_STATE_READY);
	ove_state_track_transition(&t->st, OVE_THREAD_STATE_READY);
}

#endif /* CONFIG_OVE_THREAD_STATE_STATS */
