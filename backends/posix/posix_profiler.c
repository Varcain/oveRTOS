/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * POSIX sampling profiler backend.
 *
 * The consolidated sim-debug pump calls ove_backend_profiler_sample_tick()
 * every 1/CONFIG_OVE_PROFILER_HZ seconds. Each tick snapshots the list
 * of runnable threads and delivers SIGRTMIN to each via pthread_kill().
 * The signal handler runs in the target thread's own stack, captures the
 * PC chain with glibc backtrace(3), and pushes a sample into the
 * profiler ring. The sim_profiler plugin drains the ring and forwards
 * samples to the dashboard.
 *
 * backtrace() is async-signal-safe in glibc *after* the first call
 * (which may lazy-load libgcc_s). We force that load once at
 * ove_backend_profiler_start() to keep the handler safe.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ove/profiler.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/types.h"
#include "ove/trace.h" /* for ove_backend_thread_current_handle() */
#include "ove_profiler_ring.h"

#ifndef CONFIG_OVE_PROFILER_HZ
#define CONFIG_OVE_PROFILER_HZ 250
#endif

#define PROFILER_SIG SIGRTMIN
#define PROFILER_MAX_THREADS 32

/*
 * Thread-list accessor supplied by posix_thread.c. Copies pthread_t
 * handles of all RUNNING threads into @out, returning the count.
 * Keeps thread_list_lock private to the thread module.
 */
extern size_t ove_backend_profiler_snapshot_running(pthread_t *out, size_t max);
extern void ove_backend_profiler_mark_sampled(void);
extern int ove_backend_thread_current_state(void);

static atomic_int profiler_running;

/*
 * Runtime sample-rate control. The pump ticks at CONFIG_OVE_PROFILER_HZ
 * (compile-time max); set_rate() picks an integer divisor so the actual
 * sampling rate = compile-time max / divisor. divisor == 1 means every
 * tick fires. Bounded below by 1 Hz via a cap on the divisor.
 */
static atomic_uint sample_divisor = 1;
static atomic_uint sample_counter;

static void profile_sig_handler(int sig, siginfo_t *info, void *uctx)
{
	(void)sig;
	(void)info;
	(void)uctx;

	uintptr_t handle = ove_backend_thread_current_handle();
	if (handle == 0)
		return;

	/* Preserve errno across the handler. */
	int saved_errno = errno;

	/*
	 * backtrace() called from a signal handler on Linux x86_64 glibc
	 * returns, from leaf first:
	 *   [0] inside profile_sig_handler
	 *   [1] __restore_rt (kernel sigreturn trampoline)
	 *   [2] the interrupted user-code PC
	 *   [3..] the rest of the interrupted thread's stack
	 *
	 * The first two entries are signal-delivery bookkeeping, never user
	 * code — drop them at the source. Otherwise the flat-top would be
	 * pegged at profile_sig_handler (or the symbol the bridge gap-fill
	 * happens to map __restore_rt into).
	 *
	 * Capture depth+SKIP then shift down.
	 */
	enum { HANDLER_FRAME_SKIP = 2 };
	void *pcs[CONFIG_OVE_PROFILER_MAX_DEPTH + HANDLER_FRAME_SKIP];
	int n = backtrace(pcs, CONFIG_OVE_PROFILER_MAX_DEPTH + HANDLER_FRAME_SKIP);
	if (n <= HANDLER_FRAME_SKIP) {
		ove_backend_profiler_mark_sampled();
		errno = saved_errno;
		return;
	}

	int depth = n - HANDLER_FRAME_SKIP;
	if (depth > CONFIG_OVE_PROFILER_MAX_DEPTH)
		depth = CONFIG_OVE_PROFILER_MAX_DEPTH;

	struct ove_profiler_sample s;
	memset(&s, 0, sizeof(s));
	s.ts_us = ove_state_stats_now_us();
	s.tid = (uint32_t)handle;
	s.depth = (uint8_t)depth;
	/* Capture thread state at sample time so the dashboard can
	 * filter on-CPU (RUNNING/READY) vs wall-clock (+BLOCKED) views. */
	s.state = (uint8_t)ove_backend_thread_current_state();
	for (int i = 0; i < depth; i++)
		s.pcs[i] = (uintptr_t)pcs[i + HANDLER_FRAME_SKIP];

	(void)ove_profiler_ring_push(&s);

	ove_backend_profiler_mark_sampled();
	errno = saved_errno;
}

void ove_backend_profiler_sample_tick(void)
{
	if (!atomic_load_explicit(&profiler_running, memory_order_acquire))
		return;

	/* Sub-sample when the dashboard has requested a lower rate than the
	 * compile-time max. Counter wraps harmlessly. */
	unsigned div = atomic_load_explicit(&sample_divisor, memory_order_acquire);
	if (div > 1) {
		unsigned c =
			atomic_fetch_add_explicit(&sample_counter, 1, memory_order_acq_rel) + 1;
		if ((c % div) != 0)
			return;
	}

	/* Skip self — the sim-debug pump drives this tick and sampling its
	 * own stack just reports pthread_kill/snapshot_running bookkeeping,
	 * dominating the flame graph via observer effect. */
	pthread_t self = pthread_self();
	pthread_t snap[PROFILER_MAX_THREADS];
	size_t n = ove_backend_profiler_snapshot_running(snap, PROFILER_MAX_THREADS);
	for (size_t i = 0; i < n; i++) {
		if (pthread_equal(snap[i], self))
			continue;
		(void)pthread_kill(snap[i], PROFILER_SIG);
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

/*
 * POSIX does not symbolicate on target — the bridge reads `nm -n` on the
 * ELF at sim start and forwards a full PROFILE_SUB_SYMBOLS frame. Return
 * 0 so the sim_profiler drain loop treats each tick as "no new symbols".
 */
size_t ove_backend_profiler_drain_symbols(char *out, size_t out_max)
{
	(void)out;
	(void)out_max;
	return 0;
}

static int install_signal_handler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = profile_sig_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(PROFILER_SIG, &sa, NULL) != 0) {
		fprintf(stderr, "[profiler] sigaction failed: %s\n", strerror(errno));
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

static void warmup_backtrace(void)
{
	/* Force libgcc_s load before any signal handler runs backtrace. */
	void *tmp[4];
	(void)backtrace(tmp, 4);
}

int ove_backend_profiler_start(void)
{
	int expected = 0;
	if (!atomic_compare_exchange_strong_explicit(&profiler_running, &expected, 1,
						     memory_order_acq_rel, memory_order_relaxed))
		return OVE_OK; /* already armed */

	warmup_backtrace();

	int ret = install_signal_handler();
	if (ret != OVE_OK) {
		atomic_store_explicit(&profiler_running, 0, memory_order_release);
		return ret;
	}

	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
	atomic_store_explicit(&profiler_running, 0, memory_order_release);
}

#endif /* CONFIG_OVE_PROFILER */
