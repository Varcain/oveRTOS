/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM/Emscripten thread backend.
 *
 * Identical to the POSIX backend except:
 *   - No signals (SIGUSR1 not supported in Emscripten)
 *   - Cooperative suspend via atomic flag + semaphore
 *   - No SIGINT handler in start_scheduler
 *   - No mallinfo for memory stats (use Emscripten API)
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h"
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <semaphore.h>
#include <string.h>
#include <time.h>

#ifdef CONFIG_OVE_PROFILER
extern void ove_backend_profiler_check(void);
#else
static inline void ove_backend_profiler_check(void)
{
}
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/stack.h>
#include <malloc.h>
#endif

/* Sentinel word used to paint the stack below the entry SP so the
 * high-water mark can be recovered by scanning from the bottom.  Chosen
 * distinct from 0 and from common data patterns to minimise false
 * positives when a thread legitimately writes this value. */
#define OVE_WASM_STACK_COLOR 0xDEADBEEFu

#ifdef CONFIG_OVE_THREAD_STATE_STATS
uint64_t ove_state_stats_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

/* Set thread state with tracking + trace emit. */
#define SET_STATE(t, s)                                                \
	do {                                                           \
		ove_trace_emit_state((uintptr_t)(t), (t)->state, (s)); \
		ove_state_track_transition(&(t)->st, (s));             \
		(t)->state = (s);                                      \
	} while (0)

static __thread struct ove_thread *tls_current;
static struct ove_thread *first_thread;

/* ── Thread registry for supervisor ────────────────────────────────── */

#define MAX_TRACKED_THREADS 16
static struct ove_thread *tracked_threads[MAX_TRACKED_THREADS];
static int tracked_count;
static pthread_mutex_t tracked_lock = PTHREAD_MUTEX_INITIALIZER;

static void track_add(struct ove_thread *t)
{
	pthread_mutex_lock(&tracked_lock);
	if (tracked_count < MAX_TRACKED_THREADS)
		tracked_threads[tracked_count++] = t;
	pthread_mutex_unlock(&tracked_lock);
}

static void track_remove(struct ove_thread *t)
{
	pthread_mutex_lock(&tracked_lock);
	for (int i = 0; i < tracked_count; i++) {
		if (tracked_threads[i] == t) {
			tracked_threads[i] = tracked_threads[--tracked_count];
			break;
		}
	}
	pthread_mutex_unlock(&tracked_lock);
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void touch_yield(struct ove_thread *t)
{
	if (t)
		t->last_yield_us = now_us();
}

/**
 * Check if suspension was requested and block until resumed.
 * Called from yield/sleep points for cooperative suspension.
 */
static void check_suspend(struct ove_thread *t)
{
	if (t && t->suspend_requested) {
		SET_STATE(t, OVE_THREAD_STATE_SUSPENDED);
		t->suspend_requested = 0;
		/* Block until resumed via sem_post */
		while (t->state == OVE_THREAD_STATE_SUSPENDED) {
			sem_wait(&t->suspend_sem);
		}
	}
}

/**
 * Paint the stack below the current frame with a sentinel pattern so
 * ove_thread_get_stack_usage can recover the high-water mark later.
 * Runs inside the new pthread (emscripten_stack_* only report the
 * caller's stack).  Leaves a safety margin above the current SP so the
 * local state live on entry is not clobbered.
 */
static void paint_stack(struct ove_thread *t)
{
#ifdef __EMSCRIPTEN__
	uintptr_t base = emscripten_stack_get_base();
	uintptr_t end = emscripten_stack_get_end();
	if (base <= end)
		return; /* unexpected — bail */

	t->stack_base = base;
	t->stack_end = end;
	t->stack_size = (size_t)(base - end);

	uintptr_t cur_sp = emscripten_stack_get_current();
	/* Keep a generous margin under SP so paint loop temporaries and
	 * any not-yet-stored registers stay untouched. */
	const uintptr_t margin = 512;
	if (cur_sp <= end + margin)
		return;
	uintptr_t top = cur_sp - margin;
	top &= ~(uintptr_t)(sizeof(uint32_t) - 1);

	uint32_t *p = (uint32_t *)end;
	uint32_t *stop = (uint32_t *)top;
	while (p < stop)
		*p++ = OVE_WASM_STACK_COLOR;
	t->stack_painted = 1;
#else
	(void)t;
#endif
}

static void *thread_wrapper(void *arg)
{
	struct ove_thread *t = arg;
	tls_current = t;

	paint_stack(t);

	SET_STATE(t, OVE_THREAD_STATE_RUNNING);
	t->last_yield_us = now_us();
	t->entry(t->arg);
	SET_STATE(t, OVE_THREAD_STATE_TERMINATED);
	track_remove(t);
	return NULL;
}

int ove_thread_init(ove_thread_t *handle, ove_thread_storage_t *storage,
		    const char *name, ove_thread_fn entry, void *arg,
		    ove_prio_t priority, size_t stack_size, void *stack)
{
	if (!handle || !storage || !entry)
		return OVE_ERR_INVALID_PARAM;
	(void)stack;

	struct ove_thread *t = (struct ove_thread *)storage;
	memset(t, 0, sizeof(*t));

	t->entry = entry;
	t->arg = arg;
	t->name = name;
	t->stack_size = stack_size;
	t->priority = priority;
	t->state = OVE_THREAD_STATE_READY;
	ove_state_track_init(&t->st, OVE_THREAD_STATE_READY);
	t->suspend_requested = 0;
	t->last_yield_us = now_us();
	sem_init(&t->suspend_sem, 0, 0);

	if (pthread_create(&t->tid, NULL, thread_wrapper, t) != 0) {
		sem_destroy(&t->suspend_sem);
		return OVE_ERR_NO_MEMORY;
	}

	t->started = 1;
	track_add(t);

	if (first_thread == NULL)
		first_thread = t;

	*handle = t;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	struct ove_thread *t = handle;
	if (t->started) {
		if (t->state == OVE_THREAD_STATE_SUSPENDED) {
			SET_STATE(t, OVE_THREAD_STATE_READY);
			sem_post(&t->suspend_sem);
		}
		pthread_join(t->tid, NULL);
	}
	track_remove(t);
	if (first_thread == t)
		first_thread = NULL;
	sem_destroy(&t->suspend_sem);
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_thread_create(ove_thread_t *handle, const char *name, ove_thread_fn entry,
		      void *arg, ove_prio_t priority, size_t stack_size)
{
	if (!handle || !entry)
		return OVE_ERR_INVALID_PARAM;

	struct ove_thread *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t)
		return OVE_ERR_NO_MEMORY;
	memset(t, 0, sizeof(*t));

	t->entry = entry;
	t->arg = arg;
	t->name = name;
	t->stack_size = stack_size;
	t->priority = priority;
	t->state = OVE_THREAD_STATE_READY;
	ove_state_track_init(&t->st, OVE_THREAD_STATE_READY);
	t->suspend_requested = 0;
	t->last_yield_us = now_us();
	sem_init(&t->suspend_sem, 0, 0);

	if (pthread_create(&t->tid, NULL, thread_wrapper, t) != 0) {
		sem_destroy(&t->suspend_sem);
		OVE_BACKEND_FREE(t);
		return OVE_ERR_NO_MEMORY;
	}

	t->started = 1;
	track_add(t);

	if (first_thread == NULL)
		first_thread = t;

	*handle = t;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	struct ove_thread *t = handle;
	if (t->started) {
		if (t->state == OVE_THREAD_STATE_SUSPENDED) {
			SET_STATE(t, OVE_THREAD_STATE_READY);
			sem_post(&t->suspend_sem);
		}
		pthread_join(t->tid, NULL);
	}
	if (first_thread == t)
		first_thread = NULL;
	sem_destroy(&t->suspend_sem);
	OVE_BACKEND_FREE(t);
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

ove_thread_t ove_thread_get_self(void)
{
	return tls_current;
}

void ove_thread_set_priority(ove_thread_t handle, ove_prio_t prio)
{
	(void)handle;
	(void)prio;
}

void ove_thread_sleep_ms(uint32_t ms)
{
	struct ove_thread *t = tls_current;
	touch_yield(t);
	ove_backend_profiler_check();
	check_suspend(t);
	if (t)
		SET_STATE(t, OVE_THREAD_STATE_BLOCKED);
	usleep((useconds_t)ms * 1000);
	if (t)
		SET_STATE(t, OVE_THREAD_STATE_RUNNING);
	touch_yield(t);
	check_suspend(t);
}

#ifdef CONFIG_OVE_THREAD_STATE_STATS
void ove_backend_thread_set_state(int new_state)
{
	struct ove_thread *t = tls_current;
	if (t)
		SET_STATE(t, new_state);
}
#endif

void ove_thread_yield(void)
{
	touch_yield(tls_current);
	ove_backend_profiler_check();
	check_suspend(tls_current);
	sched_yield();
}

/* ── Supervisor thread ─────────────────────────────────────────────── */

#define SUPERVISOR_INTERVAL_MS 500     /* check every 500ms */
#define UNRESPONSIVE_THRESHOLD_MS 2000 /* warn after 2s without yield */

static volatile int supervisor_running;

static void *supervisor_loop(void *arg)
{
	(void)arg;

	while (supervisor_running) {
		usleep(SUPERVISOR_INTERVAL_MS * 1000);

		uint64_t now = now_us();
		pthread_mutex_lock(&tracked_lock);
		for (int i = 0; i < tracked_count; i++) {
			struct ove_thread *t = tracked_threads[i];
			if (!t || !t->started)
				continue;
			if (t->state == OVE_THREAD_STATE_TERMINATED ||
			    t->state == OVE_THREAD_STATE_SUSPENDED)
				continue;

			uint64_t last = t->last_yield_us;
			if (last == 0)
				continue;
			uint64_t elapsed_ms = (now - last) / 1000;

			if (elapsed_ms > UNRESPONSIVE_THRESHOLD_MS) {
				/* Thread hasn't yielded — may be CPU-bound.
				 * Set suspend flag so it suspends at next
				 * opportunity. Also log a warning. */
				if (t->suspend_requested) {
					/* Already flagged — this is a repeat.
					 * The thread is truly stuck. */
					fprintf(stderr,
						"[supervisor] thread %p "
						"unresponsive for %llu ms "
						"(CPU-bound loop?)\n",
						(void *)t, (unsigned long long)elapsed_ms);
				}
			}
		}
		pthread_mutex_unlock(&tracked_lock);
	}
	return NULL;
}

static pthread_t supervisor_tid;

static void supervisor_start(void)
{
	supervisor_running = 1;
	pthread_create(&supervisor_tid, NULL, supervisor_loop, NULL);
}

void ove_thread_start_scheduler(void)
{
	/* Start the supervisor thread that monitors responsiveness. */
	supervisor_start();

	/* Block by joining the first app thread (works because
	 * -sPROXY_TO_PTHREAD runs main() in a pthread). */
	if (first_thread != NULL)
		pthread_join(first_thread->tid, NULL);

	supervisor_running = 0;
}

void ove_thread_suspend(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t && t->started) {
		/* Cooperative: set flag, thread will suspend at next
		 * yield/sleep point.  No signal delivery. */
		t->suspend_requested = 1;
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t && t->state == OVE_THREAD_STATE_SUSPENDED) {
		SET_STATE(t, OVE_THREAD_STATE_READY);
		sem_post(&t->suspend_sem);
	}
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (!t || !t->stack_painted)
		return 0;

	/* Scan from the stack limit upward for the first word the thread
	 * has touched.  Stack grows down, so the deepest usage sits at
	 * the lowest address that is no longer the sentinel.
	 *
	 * Racing with the thread is acceptable: we only read memory, and
	 * the sentinel-region is either still untouched (read-stable) or
	 * transitioning to written (which monotonically increases the
	 * computed high-water mark — never reports a false-low).
	 */
	const uint32_t *p = (const uint32_t *)t->stack_end;
	const uint32_t *top = (const uint32_t *)t->stack_base;
	while (p < top && *p == OVE_WASM_STACK_COLOR)
		p++;
	return (size_t)(t->stack_base - (uintptr_t)p);
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	return t ? t->state : OVE_THREAD_STATE_UNKNOWN;
}

int ove_thread_get_runtime_stats(ove_thread_t handle, struct ove_thread_stats *stats)
{
	(void)handle;
	if (stats) {
		stats->runtime_us = 0;
		stats->cpu_percent_x100 = 0;
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats)
		return OVE_ERR_INVALID_PARAM;
	memset(stats, 0, sizeof(*stats));
#ifdef __EMSCRIPTEN__
	/* Emscripten/emmalloc mallinfo returns int fields (not size_t) and
	 * does not track peak usage. We track peak locally as a monotonic
	 * max of observed `used` across calls. */
	static size_t peak_used;
	struct mallinfo mi = mallinfo();
	stats->total = (size_t)emscripten_get_heap_size();
	stats->used = (size_t)(unsigned)mi.uordblks;
	stats->free = (size_t)(unsigned)mi.fordblks;
	if (stats->used > peak_used)
		peak_used = stats->used;
	stats->peak_used = peak_used;
#endif
	return OVE_OK;
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count, size_t *actual_count)
{
	if (!out) {
		if (actual_count)
			*actual_count = 0;
		return OVE_OK;
	}

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	uint64_t running_us[MAX_TRACKED_THREADS] = {0};
	uint64_t total_running_us = 0;
#endif
	size_t count = 0;

	pthread_mutex_lock(&tracked_lock);
	int n = tracked_count;
	if ((size_t)n > max_count)
		n = (int)max_count;
	for (int i = 0; i < n; i++) {
		struct ove_thread *t = tracked_threads[i];
		if (!t)
			continue;
		out[count].name = t->name ? t->name : "?";
		out[count].state = (ove_thread_state_t)t->state;
		out[count].priority = t->priority;
		out[count].stack_used = t->stack_painted ? ove_thread_get_stack_usage(t) : 0;
		out[count].stack_size = t->stack_size;
		out[count].cpu_percent_x100 = 0;
#ifdef CONFIG_OVE_THREAD_STATE_STATS
		/* Fold an in-progress RUNNING slice into the snapshot so
		 * CPU% reflects current activity, not just past transitions. */
		uint64_t cumul[OVE_STATE_COUNT];
		for (int k = 0; k < OVE_STATE_COUNT; k++)
			cumul[k] = t->st.cumul_us[k];
		uint64_t now = ove_state_stats_now_us();
		int cur = t->st.cur_state;
		if (cur >= 0 && cur < OVE_STATE_COUNT && now > t->st.last_ts_us)
			cumul[cur] += now - t->st.last_ts_us;

		out[count].state_times.running_us = cumul[0];
		out[count].state_times.ready_us = cumul[1];
		out[count].state_times.blocked_us = cumul[2];
		out[count].state_times.suspended_us = cumul[3];
		running_us[count] = cumul[0];
		total_running_us += cumul[0];
#else
		memset(&out[count].state_times, 0, sizeof(out[count].state_times));
#endif
		count++;
	}
	pthread_mutex_unlock(&tracked_lock);

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	if (total_running_us > 0) {
		for (size_t i = 0; i < count; i++)
			out[i].cpu_percent_x100 =
				(uint32_t)(running_us[i] * 10000ULL / total_running_us);
	}
#endif

	if (actual_count)
		*actual_count = count;
	return OVE_OK;
}

#ifdef CONFIG_OVE_TRACE_STREAM
#include "ove_trace_ring.h"

uintptr_t ove_backend_thread_current_handle(void)
{
	return (uintptr_t)tls_current;
}

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max)
{
	size_t count = 0;
	pthread_mutex_lock(&tracked_lock);
	for (int i = 0; i < tracked_count && count < max; i++) {
		struct ove_thread *t = tracked_threads[i];
		if (!t)
			continue;
		out[count].tid = (uint32_t)(uintptr_t)t;
		out[count].name = t->name ? t->name : "?";
		count++;
	}
	pthread_mutex_unlock(&tracked_lock);
	return count;
}
#endif /* CONFIG_OVE_TRACE_STREAM */

#ifdef CONFIG_OVE_PROFILER
/*
 * Exports for the WASM profiler backend (wasm_profiler.c).
 *
 * In the WASM model the pump (sim_profiler tick) flags every RUNNING
 * thread each sample period; each flagged thread self-captures its
 * callstack at its next yield point. No signals are available under
 * Emscripten pthreads, so this supervisor-flag scheme replaces the
 * POSIX SIGRTMIN + backtrace(3) approach.
 */

struct ove_thread *ove_backend_thread_current_struct(void)
{
	return tls_current;
}

size_t ove_backend_profiler_flag_running(void)
{
	size_t flagged = 0;
	pthread_mutex_lock(&tracked_lock);
	for (int i = 0; i < tracked_count; i++) {
		struct ove_thread *t = tracked_threads[i];
		if (!t || !t->started)
			continue;
		if (t->state != OVE_THREAD_STATE_RUNNING && t->state != OVE_THREAD_STATE_READY &&
		    t->state != OVE_THREAD_STATE_BLOCKED)
			continue;
		t->profiler_pending = 1;
		flagged++;
	}
	pthread_mutex_unlock(&tracked_lock);
	return flagged;
}
#endif /* CONFIG_OVE_PROFILER */
