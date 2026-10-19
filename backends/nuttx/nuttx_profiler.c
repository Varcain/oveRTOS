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
 * Unlike FreeRTOS and Zephyr (ISR-driven PSP read + saved-{r7,lr} stack
 * scan), NuttX ships a portable on-target unwinder: up_backtrace(tcb,
 * buf, size, skip). It is enabled by CONFIG_SCHED_BACKTRACE=y and works
 * on any architecture with .eh_frame or a frame-pointer convention
 * (ARMv7-M is supported).
 *
 * up_backtrace(tcb, ...) walks the SAVED register context of @tcb, so it's
 * safe to call for any TCB that isn't currently running — the scheduler's
 * stack-saved {r7, lr} / PSP state is authoritative after the last context
 * switch.
 *
 * Sampling is timed by a NuttX watchdog (ISR context, re-armed on an
 * absolute tick schedule, so the rate is exact) that queues one work item
 * on the high-priority work queue. The HPWORK worker (native priority
 * CONFIG_SCHED_HPWORKPRIORITY, 224) runs above every ove thread up to
 * OVE_PRIO_ABOVE(OVE_PRIO_REALTIME, 4), so busy threads are sampled too:
 * the thread it preempted is captured with its real stack, in state
 * READY. Each tick walks every RUNNING/READY/BLOCKED ove thread in the
 * registry (populated by nuttx_thread.c).
 *
 * Symbolication: host-side via the dashboard bridge (arm-none-eabi-nm on
 * the firmware ELF).
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "ove/profiler.h"
#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h" /* ove_backend_thread_current_handle */
#include "ove/types.h"
#include "ove_profiler_ring.h"
#include "ove_thread_registry.h"

#ifndef CONFIG_OVE_PROFILER_HZ
#define CONFIG_OVE_PROFILER_HZ 250
#endif

/* The thread registry (ove_thread_registry.h), populated by nuttx_thread.c;
 * trace descriptors use nxsched_foreach instead. The worker snapshots PIDs
 * under the list lock (taken with trylock: a high-priority worker must not
 * wait behind a lower-priority thread holding it), then releases it before
 * calling up_backtrace. */
extern bool ove_nuttx_thread_registry_trylock(void);

#define PROFILER_MAX_THREADS 32

static atomic_int profiler_running;
static atomic_uint period_ticks; /* 0 = not set yet */

static struct wdog_s sample_wdog;
static struct work_s sample_work;
static clock_t next_tick;
/* Set while the walk runs: the work queue clears the item before calling
 * it, so work_available() alone would let a tick re-queue a running walk
 * and a walk longer than the period would keep HPWORK busy for good. */
static atomic_bool walk_busy;

struct snap_entry {
	struct ove_thread *t;
	pid_t pid;
	int state;
};

static size_t snapshot_threads(struct snap_entry *out, size_t max)
{
	size_t n = 0;
	for (struct ove_thread *t = ove_backend_thread_registry_head; t && n < max; t = t->next) {
		if (!t->started)
			continue;
		/* Scheduler-observed state (the profiler depends on
		 * OVE_THREAD_STATE_STATS); t->state only tracks suspension.
		 * Sample RUNNING/READY/BLOCKED: a blocked task's saved stack
		 * is the waiter call site, usually what the user wants. */
		int s = t->st.cur_state;
		if (s != OVE_THREAD_STATE_RUNNING && s != OVE_THREAD_STATE_READY &&
		    s != OVE_THREAD_STATE_BLOCKED)
			continue;
		out[n].t = t;
		out[n].pid = t->pid;
		out[n].state = s;
		n++;
	}
	return n;
}

/* HPWORK context: walk every tracked thread's saved context. */
static void sample_walk(void)
{
	if (!atomic_load_explicit(&profiler_running, memory_order_acquire))
		return;

	struct snap_entry snap[PROFILER_MAX_THREADS];
	if (!ove_nuttx_thread_registry_trylock()) {
		ove_profiler_ring_add_dropped(1);
		return;
	}
	size_t n = snapshot_threads(snap, PROFILER_MAX_THREADS);
	ove_backend_thread_registry_unlock();

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

static void sample_worker(FAR void *arg)
{
	(void)arg;
	atomic_store_explicit(&walk_busy, true, memory_order_relaxed);
	sample_walk();
	atomic_store_explicit(&walk_busy, false, memory_order_release);
}

/* Watchdog (ISR context): re-arm on an absolute schedule so the period
 * does not drift, then hand the walk to HPWORK. A tick whose previous walk
 * is still queued or running is counted as dropped. */
static void sample_wdog_cb(wdparm_t arg)
{
	(void)arg;
	if (!atomic_load_explicit(&profiler_running, memory_order_acquire))
		return;

	next_tick += atomic_load_explicit(&period_ticks, memory_order_relaxed);
	wd_start_abstick(&sample_wdog, next_tick, sample_wdog_cb, 0);

	if (work_available(&sample_work) && !atomic_load_explicit(&walk_busy, memory_order_acquire))
		work_queue(HPWORK, &sample_work, sample_worker, NULL, 0);
	else
		ove_profiler_ring_add_dropped(1);
}

static unsigned int ticks_for_hz(uint32_t hz)
{
	unsigned int ticks = (unsigned int)((TICK_PER_SEC + hz / 2u) / hz);
	return ticks ? ticks : 1u;
}

void ove_backend_profiler_set_rate(uint32_t hz)
{
	uint32_t max_hz = ove_backend_profiler_get_max_hz();
	if (hz == 0 || hz > max_hz)
		hz = max_hz;
	atomic_store_explicit(&period_ticks, ticks_for_hz(hz), memory_order_relaxed);
}

uint32_t ove_backend_profiler_get_flags(void)
{
	/* Samples every ove thread, blocked ones included. */
	return OVE_PROFILER_FLAG_WALL_CLOCK;
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	/* Can't sample faster than the system tick. */
	uint32_t cfg_hz = (uint32_t)CONFIG_OVE_PROFILER_HZ;
	uint32_t tick_hz = (uint32_t)TICK_PER_SEC;
	return (cfg_hz < tick_hz) ? cfg_hz : tick_hz;
}

int ove_backend_profiler_start(void)
{
	if (atomic_exchange_explicit(&profiler_running, 1, memory_order_acq_rel))
		return OVE_OK; /* already running */
	if (atomic_load_explicit(&period_ticks, memory_order_relaxed) == 0u)
		ove_backend_profiler_set_rate(ove_backend_profiler_get_max_hz());
	next_tick =
		clock_systime_ticks() + atomic_load_explicit(&period_ticks, memory_order_relaxed);
	int ret = wd_start_abstick(&sample_wdog, next_tick, sample_wdog_cb, 0);
	if (ret < 0) {
		atomic_store_explicit(&profiler_running, 0, memory_order_release);
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
	atomic_store_explicit(&profiler_running, 0, memory_order_release);
	wd_cancel(&sample_wdog);
	work_cancel(HPWORK, &sample_work);
}

#endif /* CONFIG_OVE_PROFILER */
