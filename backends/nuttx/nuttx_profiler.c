/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * NuttX sampling profiler backend.
 *
 * Unlike POSIX (SIGRTMIN + glibc backtrace in each target's signal handler)
 * or FreeRTOS (ISR-driven PSP read + FP-chain scan), NuttX ships a portable
 * on-target unwinder: up_backtrace(tcb, buf, size, skip). It is enabled by
 * CONFIG_SCHED_BACKTRACE=y and works on any architecture with .eh_frame or
 * a frame-pointer convention (ARMv7-M is supported).
 *
 * up_backtrace(tcb, ...) walks the SAVED register context of @tcb, so it's
 * safe to call for any TCB that isn't currently running — the scheduler's
 * stack-saved {r7, lr} / PSP state is authoritative after the last context
 * switch. For the current task we pass NULL and the unwinder walks live
 * registers.
 *
 * Sampling is driven by the sim-debug pump calling sample_tick() at
 * CONFIG_OVE_PROFILER_HZ. Each tick iterates the thread registry (populated
 * by nuttx_thread.c) and produces one sample per runnable ove_thread. The
 * pump's own task is skipped so the flame graph isn't dominated by the
 * observer.
 *
 * Symbolication: host-side via the dashboard bridge (arm-none-eabi-nm on
 * the firmware ELF). drain_symbols() returns 0.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <nuttx/arch.h>

#include "ove/profiler.h"
#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h" /* ove_backend_thread_current_handle */
#include "ove/types.h"
#include "ove_profiler_ring.h"

#ifndef CONFIG_OVE_PROFILER_HZ
#define CONFIG_OVE_PROFILER_HZ 250
#endif

/* Thread backend exports the registry + locks — same accessors used by
 * nuttx_trace.c. We iterate under the list lock to snapshot PIDs into a
 * small stack array, then release the lock before calling up_backtrace so
 * the unwinder can freely acquire scheduler locks. */
extern struct ove_thread *ove_nuttx_thread_list_head;
extern void ove_nuttx_thread_list_lock(void);
extern void ove_nuttx_thread_list_unlock(void);
extern struct ove_thread *ove_nuttx_current_thread(void);

#define PROFILER_MAX_THREADS 32

static atomic_int profiler_running;

/* sample_tick is called by the pump at CONFIG_OVE_PROFILER_HZ. The divisor
 * drops the effective rate when the dashboard requests a slower cadence. */
static atomic_uint sample_divisor = 1;
static atomic_uint sample_counter;

struct snap_entry {
	struct ove_thread *t;
	pid_t pid;
	int state;
};

static size_t snapshot_runnable(struct snap_entry *out, size_t max, struct ove_thread *skip)
{
	size_t n = 0;
	ove_nuttx_thread_list_lock();
	for (struct ove_thread *t = ove_nuttx_thread_list_head; t && n < max; t = t->next) {
		if (t == skip || !t->started)
			continue;
		int s = t->state;
		/* Same policy as POSIX: sample RUNNING/READY/BLOCKED. A
		 * blocked task's saved stack is the waiter call site, which
		 * is usually what the user wants to see. */
		if (s != OVE_THREAD_STATE_RUNNING && s != OVE_THREAD_STATE_READY &&
		    s != OVE_THREAD_STATE_BLOCKED)
			continue;
		out[n].t = t;
		out[n].pid = t->pid;
		out[n].state = s;
		n++;
	}
	ove_nuttx_thread_list_unlock();
	return n;
}

void ove_backend_profiler_sample_tick(void)
{
	if (!atomic_load_explicit(&profiler_running, memory_order_acquire))
		return;

	unsigned div = atomic_load_explicit(&sample_divisor, memory_order_acquire);
	if (div > 1) {
		unsigned c =
			atomic_fetch_add_explicit(&sample_counter, 1, memory_order_acq_rel) + 1;
		if ((c % div) != 0)
			return;
	}

	/* Skip the pump task so the flame graph isn't pegged at
	 * sim_debug.c:debug_thread_fn. */
	struct ove_thread *self = ove_nuttx_current_thread();

	struct snap_entry snap[PROFILER_MAX_THREADS];
	size_t n = snapshot_runnable(snap, PROFILER_MAX_THREADS, self);
	uint64_t now = ove_state_stats_now_us();

	for (size_t i = 0; i < n; i++) {
		struct tcb_s *tcb = nxsched_get_tcb(snap[i].pid);
		if (!tcb)
			continue; /* pid reaped between snapshot and walk */

		void *pcs[CONFIG_OVE_PROFILER_MAX_DEPTH];
		int depth = up_backtrace(tcb, pcs, CONFIG_OVE_PROFILER_MAX_DEPTH, 0);
		if (depth <= 0)
			continue;
		if (depth > CONFIG_OVE_PROFILER_MAX_DEPTH)
			depth = CONFIG_OVE_PROFILER_MAX_DEPTH;

		struct ove_profiler_sample s;
		memset(&s, 0, sizeof(s));
		s.ts_us = now;
		s.tid = (uint32_t)(uintptr_t)snap[i].t;
		s.state = (uint8_t)snap[i].state;
		s.depth = (uint8_t)depth;
		for (int k = 0; k < depth; k++)
			s.pcs[k] = (uintptr_t)pcs[k];

		(void)ove_profiler_ring_push(&s);
	}
}

void ove_backend_profiler_set_rate(uint32_t hz)
{
	if (hz == 0 || hz > (uint32_t)CONFIG_OVE_PROFILER_HZ)
		hz = (uint32_t)CONFIG_OVE_PROFILER_HZ;
	unsigned div = (unsigned)((uint32_t)CONFIG_OVE_PROFILER_HZ / hz);
	if (div == 0)
		div = 1;
	atomic_store_explicit(&sample_divisor, div, memory_order_release);
	atomic_store_explicit(&sample_counter, 0, memory_order_release);
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	return (uint32_t)CONFIG_OVE_PROFILER_HZ;
}

int ove_backend_profiler_start(void)
{
	atomic_store_explicit(&profiler_running, 1, memory_order_release);
	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
	atomic_store_explicit(&profiler_running, 0, memory_order_release);
}

size_t ove_backend_profiler_drain_symbols(char *out, size_t out_max)
{
	(void)out;
	(void)out_max;
	return 0;
}

#endif /* CONFIG_OVE_PROFILER */
