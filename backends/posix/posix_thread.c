/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h"
#include "posix_sleep.h"
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef CONFIG_OVE_THREAD_STATE_STATS
uint64_t ove_state_stats_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

/* Set thread state with tracking + trace emit.
 *
 * The store is a release: t->state is read by the destroying / enumerating
 * thread on a different CPU than the worker that writes it (TSan caught
 * exactly this — backends/posix/posix_thread.c:295 destroy-path read of
 * t->state racing with worker's SET_STATE writes at TASK_RUNNING /
 * TASK_TERMINATED).  The companion reads use __atomic_load_n with acquire
 * ordering so the post-state visibility on the reader side is well-defined
 * across all C11/C++11 compilers without dragging in <stdatomic.h>.
 *
 * The trace-emit `from` value uses a relaxed load: it is read by the
 * writing thread itself (this macro runs on the worker for state
 * transitions and on the controller for SUSPENDED/READY toggles), so no
 * cross-thread happens-before is needed for the snapshot.
 */
#define SET_STATE(t, s)                                                                    \
	do {                                                                               \
		ove_trace_emit_state((uintptr_t)(t),                                       \
				     __atomic_load_n(&(t)->state, __ATOMIC_RELAXED), (s)); \
		ove_state_track_transition(&(t)->st, (s));                                 \
		__atomic_store_n(&(t)->state, (s), __ATOMIC_RELEASE);                      \
	} while (0)
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <string.h>

static __thread struct ove_thread *tls_current;
static struct ove_thread *first_thread;

/* ── Stack coloration ─────────────────────────────────────────────── */

#define STACK_COLOR 0xDEADBEEFu
/* glibc's dynamic minimum stack for `pthread_attr_setstack` grows with
 * the binary's TLS footprint; under Zig 0.16 toolchains it lands around
 * ~272 KiB on x86_64-linux (vs ~16 KiB under Zig 0.15 and the C-only
 * baseline).  Bumped to 512 KiB to give comfortable headroom across
 * future glibc/TLS-layout drift.  This only inflates the painted-stack
 * paint buffer on the host posix backend; embedded RTOS backends
 * honour the caller-supplied stack verbatim. */
#define STACK_MIN_SIZE (512 * 1024)

#ifndef CONFIG_OVE_ZERO_HEAP
static void *_alloc_painted_stack(size_t requested, size_t *actual)
{
	/* Ensure minimum size for pthread (includes guard page). */
	size_t sz = requested < STACK_MIN_SIZE ? STACK_MIN_SIZE : requested;
	/* Align to page boundary for pthread_attr_setstack. */
	sz = (sz + 4095) & ~(size_t)4095;
	void *base = aligned_alloc(4096, sz);
	if (!base) {
		*actual = 0;
		return NULL;
	}
	/* Paint with sentinel pattern. */
	uint32_t *p = (uint32_t *)base;
	for (size_t i = 0; i < sz / sizeof(uint32_t); i++)
		p[i] = STACK_COLOR;
	*actual = sz;
	return base;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static size_t _check_stack_hwm(void *base, size_t size)
{
	/* Scan from bottom (low address) looking for first clobbered word.
	 * Stack grows downward: bottom is painted, top is used. */
	const uint32_t *p = (const uint32_t *)base;
	size_t words = size / sizeof(uint32_t);
	size_t clean = 0;
	for (size_t i = 0; i < words; i++) {
		if (p[i] != STACK_COLOR)
			break;
		clean++;
	}
	return size - clean * sizeof(uint32_t);
}

/* Simple linked list of all live threads for ove_thread_list(). */
static struct ove_thread *thread_list_head;
static pthread_mutex_t thread_list_lock = PTHREAD_MUTEX_INITIALIZER;

static void _register_thread(struct ove_thread *t)
{
	pthread_mutex_lock(&thread_list_lock);
	t->next = thread_list_head;
	thread_list_head = t;
	pthread_mutex_unlock(&thread_list_lock);
}

static void _unregister_thread(struct ove_thread *t)
{
	pthread_mutex_lock(&thread_list_lock);
	struct ove_thread **pp = &thread_list_head;
	while (*pp) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
		pp = &(*pp)->next;
	}
	pthread_mutex_unlock(&thread_list_lock);
}

static void sigusr1_handler(int sig)
{
	(void)sig;
	struct ove_thread *t = tls_current;
	if (t) {
		SET_STATE(t, OVE_THREAD_STATE_SUSPENDED);
		/* Block until resumed via sem_post — acquire-load pairs with
		 * SET_STATE's release-store so the resumer's READY transition
		 * is visible to the suspended thread on wake. */
		while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED) {
			sem_wait(&t->suspend_sem);
		}
	}
}

static void *thread_wrapper(void *arg)
{
	struct ove_thread *t = arg;
	tls_current = t;

	/* Install signal handler for suspend */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigusr1_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);

	SET_STATE(t, OVE_THREAD_STATE_RUNNING);
	t->entry(t->arg);
	SET_STATE(t, OVE_THREAD_STATE_TERMINATED);
	return NULL;
}

int ove_thread_init(ove_thread_t *handle, ove_thread_storage_t *storage, const char *name,
		    ove_thread_fn entry, void *arg, ove_prio_t priority, size_t stack_size,
		    void *stack)
{
	if (!handle || !storage || !entry) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* Backstop the AAPCS 8-byte stack alignment promised by the
	 * OVE_THREAD_STACK_* helpers in include/ove/storage.h. */
	if (stack != NULL && ((uintptr_t)stack & 7u) != 0u) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* Phase 5 / storage hygiene audit: this is a host-mode (POSIX
	 * pthreads) backend with no zero-heap obligation.  pthread_create
	 * manages its own stack via the host kernel, so the caller-supplied
	 * `stack` buffer is intentionally unused.  We additionally allocate
	 * a paint-buffer below for stack-coloration HWM tracking — also
	 * heap-allocated, since malloc is always available on the host.
	 * For zero-heap deployments use FreeRTOS or Zephyr, which honor
	 * the caller-supplied stack. */
	(void)stack;

	struct ove_thread *t = (struct ove_thread *)storage;
	memset(t, 0, sizeof(*t));

	t->entry = entry;
	t->arg = arg;
	SET_STATE(t, OVE_THREAD_STATE_READY);
	ove_state_track_init(&t->st, OVE_THREAD_STATE_READY);
	t->name = name;
	t->priority = (uint8_t)priority;
	sem_init(&t->suspend_sem, 0, 0);

	/* Allocate and paint stack for coloration-based HWM tracking.
	 * In zero-heap mode aligned_alloc is forbidden; pthread allocates
	 * its own default stack and HWM reporting drops to 0. */
	size_t actual_sz = 0;
#ifndef CONFIG_OVE_ZERO_HEAP
	t->stack_base = _alloc_painted_stack(stack_size, &actual_sz);
#else
	t->stack_base = NULL;
	(void)stack_size;
#endif
	t->stack_size = actual_sz;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	if (t->stack_base)
		pthread_attr_setstack(&attr, t->stack_base, actual_sz);

	if (pthread_create(&t->tid, &attr, thread_wrapper, t) != 0) {
		pthread_attr_destroy(&attr);
		sem_destroy(&t->suspend_sem);
		free(t->stack_base);
		t->stack_base = NULL;
		return OVE_ERR_NO_MEMORY;
	}
	pthread_attr_destroy(&attr);

	/* Release-store: the worker thread that's already running this
	 * struct via thread_wrapper() can observe started=1 with the
	 * register-thread side effects intact. */
	__atomic_store_n(&t->started, 1, __ATOMIC_RELEASE);
	_register_thread(t);

	if (first_thread == NULL) {
		first_thread = t;
	}

	*handle = t;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	struct ove_thread *t = handle;
	if (__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) {
		if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED) {
			SET_STATE(t, OVE_THREAD_STATE_READY);
			sem_post(&t->suspend_sem);
		}
		pthread_join(t->tid, NULL);
	}
	_unregister_thread(t);
	if (first_thread == t) {
		first_thread = NULL;
	}
	sem_destroy(&t->suspend_sem);
	free(t->stack_base);
	t->stack_base = NULL;
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_thread_create(ove_thread_t *handle, const char *name, ove_thread_fn entry, void *arg,
		      ove_prio_t priority, size_t stack_size)
{
	if (!handle || !entry) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_thread *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(t, 0, sizeof(*t));

	t->entry = entry;
	t->arg = arg;
	SET_STATE(t, OVE_THREAD_STATE_READY);
	ove_state_track_init(&t->st, OVE_THREAD_STATE_READY);
	t->name = name;
	t->priority = (uint8_t)priority;
	sem_init(&t->suspend_sem, 0, 0);

	size_t actual_sz = 0;
	t->stack_base = _alloc_painted_stack(stack_size, &actual_sz);
	t->stack_size = actual_sz;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	if (t->stack_base)
		pthread_attr_setstack(&attr, t->stack_base, actual_sz);

	if (pthread_create(&t->tid, &attr, thread_wrapper, t) != 0) {
		pthread_attr_destroy(&attr);
		sem_destroy(&t->suspend_sem);
		free(t->stack_base);
		t->stack_base = NULL;
		OVE_BACKEND_FREE(t);
		return OVE_ERR_NO_MEMORY;
	}
	pthread_attr_destroy(&attr);

	__atomic_store_n(&t->started, 1, __ATOMIC_RELEASE);
	_register_thread(t);

	if (first_thread == NULL) {
		first_thread = t;
	}

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
	if (__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) {
		/* Resume if suspended so it can finish */
		if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED) {
			SET_STATE(t, OVE_THREAD_STATE_READY);
			sem_post(&t->suspend_sem);
		}
		pthread_join(t->tid, NULL);
	}
	_unregister_thread(t);
	if (first_thread == t) {
		first_thread = NULL;
	}
	sem_destroy(&t->suspend_sem);
	free(t->stack_base);
	t->stack_base = NULL;
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
	/* POSIX thread priorities need CAP_SYS_NICE for anything other
	 * than SCHED_OTHER, so the OS-level priority stays as-is. We
	 * still record the requested value so ove_thread_list reports
	 * it and backend-neutral code sees a round-trip. */
	struct ove_thread *t = handle;
	if (t)
		t->priority = (uint8_t)prio;
}

void ove_thread_sleep_ms(uint32_t ms)
{
	struct ove_thread *t = tls_current;
	if (t)
		SET_STATE(t, OVE_THREAD_STATE_BLOCKED);
	posix_sleep_ms(ms);
	if (t)
		SET_STATE(t, OVE_THREAD_STATE_RUNNING);
}

#ifdef CONFIG_OVE_THREAD_STATE_STATS
void ove_backend_thread_set_state(int new_state)
{
	struct ove_thread *t = tls_current;
	if (t)
		SET_STATE(t, new_state);
}
#endif

#ifdef CONFIG_OVE_TRACE_STREAM
#include "ove_trace_ring.h"
uintptr_t ove_backend_thread_current_handle(void)
{
	return (uintptr_t)tls_current;
}

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max)
{
	size_t count = 0;
	pthread_mutex_lock(&thread_list_lock);
	for (struct ove_thread *t = thread_list_head; t && count < max; t = t->next) {
		out[count].tid = (uint32_t)(uintptr_t)t;
		out[count].name = t->name ? t->name : "?";
		count++;
	}
	pthread_mutex_unlock(&thread_list_lock);
	return count;
}
#endif

#ifdef CONFIG_OVE_PROFILER
size_t ove_backend_profiler_snapshot_running(pthread_t *out, size_t max)
{
	/*
	 * Sample every live thread, not just RUNNING. On POSIX most
	 * threads are BLOCKED in condvars/sleep most of the time — if
	 * we only signalled RUNNING threads the effective sample rate
	 * drops to <1 Hz in LVGL-style apps. A blocked thread's
	 * backtrace still gives a meaningful user-code stack (its last
	 * call into the blocking primitive), which is usually what
	 * users actually want to see.
	 *
	 * The per-thread `profiler_pending` flag is test-and-set here
	 * to avoid queuing SIGRTMIN on threads that haven't drained
	 * their previous sample yet (which would happen for long-blocked
	 * threads at 250 Hz and eventually overflow the kernel's rt-sig
	 * queue). Handler clears the flag after sampling.
	 */
	size_t count = 0;
	pthread_mutex_lock(&thread_list_lock);
	for (struct ove_thread *t = thread_list_head; t && count < max; t = t->next) {
		if (!__atomic_load_n(&t->started, __ATOMIC_ACQUIRE))
			continue;
		int s = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
		if (s != OVE_THREAD_STATE_RUNNING && s != OVE_THREAD_STATE_READY &&
		    s != OVE_THREAD_STATE_BLOCKED)
			continue;
		if (__atomic_exchange_n(&t->profiler_pending, 1, __ATOMIC_ACQ_REL))
			continue; /* already pending */
		out[count++] = t->tid;
	}
	pthread_mutex_unlock(&thread_list_lock);
	return count;
}

void ove_backend_profiler_mark_sampled(void)
{
	struct ove_thread *t = tls_current;
	if (t)
		__atomic_store_n(&t->profiler_pending, 0, __ATOMIC_RELEASE);
}

int ove_backend_thread_current_state(void)
{
	struct ove_thread *t = tls_current;
	return t ? __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) : OVE_THREAD_STATE_UNKNOWN;
}
#endif

void ove_thread_yield(void)
{
	sched_yield();
}

static void sigint_handler(int sig)
{
	(void)sig;
	_exit(0);
}

void ove_thread_start_scheduler(void)
{
	/* Allow Ctrl-C to terminate the process cleanly.  Without this,
	 * SIGINT interrupts pthread_join but the spawned threads keep the
	 * process alive because they are blocked in sleep/recv loops.
	 * signal() may return SIG_ERR; we don't propagate that here because
	 * a missing handler just falls back to the default SIGINT behaviour
	 * (process termination), which is what we want anyway. */
	(void)signal(SIGINT, sigint_handler);

	/* Block the main thread by joining the first app thread.
	 * POSIX threads run immediately, but the caller (ove_app_run)
	 * needs to block here to keep the process alive. */
	if (first_thread != NULL) {
		pthread_join(first_thread->tid, NULL);
	}
}

void ove_thread_suspend(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t && __atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) {
		pthread_kill(t->tid, SIGUSR1);
		posix_sleep_ns(1000000ULL); /* 1 ms for signal delivery */
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t && __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED) {
		SET_STATE(t, OVE_THREAD_STATE_READY);
		sem_post(&t->suspend_sem);
	}
}

void ove_thread_request_stop(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t)
		__atomic_store_n(&t->stop_requested, 1, __ATOMIC_RELEASE);
}

bool ove_thread_should_stop(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (!t)
		return false;
	return __atomic_load_n(&t->stop_requested, __ATOMIC_ACQUIRE) != 0;
}

int ove_thread_get_stack_headroom(ove_thread_t handle, size_t *headroom_bytes)
{
	if (!headroom_bytes)
		return OVE_ERR_INVALID_PARAM;
	*headroom_bytes = 0;
	if (!handle)
		return OVE_ERR_INVALID_PARAM;

	struct ove_thread *t = handle;
	if (!t->stack_base || !t->stack_size)
		return OVE_ERR_NOT_SUPPORTED;
	size_t used = _check_stack_hwm(t->stack_base, t->stack_size);
	*headroom_bytes = t->stack_size > used ? t->stack_size - used : 0;
	return OVE_OK;
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	size_t headroom = 0;
	return ove_thread_get_stack_headroom(handle, &headroom) == OVE_OK ? headroom : 0;
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t) {
		return __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
	}
	return OVE_THREAD_STATE_UNKNOWN;
}

int ove_thread_get_runtime_stats(ove_thread_t handle, struct ove_thread_stats *stats)
{
	(void)handle;
	if (stats != NULL) {
		stats->runtime_us = 0;
		stats->cpu_percent_x100 = 0;
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats)
		return OVE_ERR_INVALID_PARAM;

#ifdef __GLIBC__
	struct mallinfo2 mi = mallinfo2();
	stats->total = mi.arena;
	stats->used = mi.uordblks;
	stats->free = mi.fordblks;
	stats->peak_used = 0; /* glibc doesn't track peak */
#else
	memset(stats, 0, sizeof(*stats));
#endif
	return OVE_OK;
}

static uint64_t _thread_cpu_ns(pthread_t tid)
{
	clockid_t cid;
	struct timespec ts;
	if (pthread_getcpuclockid(tid, &cid) != 0)
		return 0;
	if (clock_gettime(cid, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count, size_t *actual_count)
{
	if (!out) {
		if (actual_count)
			*actual_count = 0;
		return OVE_OK;
	}

	/* CPU% = (per-thread CPU-time delta) / (wall-time delta), i.e. the
	 * fraction of one core the thread consumed since the previous
	 * sample. We only refresh the cached percentages when enough wall
	 * time has elapsed (100 ms) — rapid back-to-back callers (e.g. the
	 * sim_debug pump + an httpd handler firing in the same window)
	 * would otherwise collapse the delta window to ~0 and the numbers
	 * would jitter wildly. */
	static uint64_t prev_wall_ns;
	struct timespec wts;
	clock_gettime(CLOCK_MONOTONIC, &wts);
	uint64_t wall_ns = (uint64_t)wts.tv_sec * 1000000000ULL + (uint64_t)wts.tv_nsec;

	pthread_mutex_lock(&thread_list_lock);
	uint64_t delta_wall = (prev_wall_ns && wall_ns > prev_wall_ns) ? (wall_ns - prev_wall_ns)
								       : 0;
	int refresh = (delta_wall >= 100000000ULL); /* 100 ms */

	size_t count = 0;
	struct ove_thread *t = thread_list_head;
	while (t && count < max_count) {
		uint64_t cpu_ns = _thread_cpu_ns(t->tid);
		if (refresh) {
			int sample_valid = t->cpu_prev_ns && cpu_ns >= t->cpu_prev_ns;
			uint64_t dcpu = sample_valid ? cpu_ns - t->cpu_prev_ns : 0;
			t->cpu_pct_x100 = (uint32_t)(dcpu * 10000ULL / delta_wall);
			t->cpu_pct_valid = (uint8_t)sample_valid;
			t->cpu_prev_ns = cpu_ns;
		} else if (!t->cpu_prev_ns) {
			/* First observation ever — seed so the next refresh
			 * produces a valid delta. */
			t->cpu_prev_ns = cpu_ns;
		}

		out[count].name = t->name ? t->name : "?";
		out[count].identity = (uintptr_t)t;
		out[count].state = (ove_thread_state_t)__atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
		out[count].priority = t->priority;
		out[count].stack_used =
			t->stack_base ? _check_stack_hwm(t->stack_base, t->stack_size) : 0;
		out[count].stack_size = t->stack_size;
		out[count].cpu_percent_x100 = t->cpu_pct_x100;
		out[count].valid_fields = 0;
		if (t->stack_base && t->stack_size)
			out[count].valid_fields |= OVE_THREAD_INFO_VALID_STACK_USED |
						   OVE_THREAD_INFO_VALID_STACK_SIZE;
		if (t->cpu_pct_valid)
			out[count].valid_fields |= OVE_THREAD_INFO_VALID_CPU_PERCENT;
#ifdef CONFIG_OVE_THREAD_STATE_STATS
		out[count].state_times.running_us = t->st.cumul_us[0];
		out[count].state_times.ready_us = t->st.cumul_us[1];
		out[count].state_times.blocked_us = t->st.cumul_us[2];
		out[count].state_times.suspended_us = t->st.cumul_us[3];
		out[count].valid_fields |= OVE_THREAD_INFO_VALID_STATE_TIMES;
#else
		memset(&out[count].state_times, 0, sizeof(out[count].state_times));
#endif
		count++;
		t = t->next;
	}

	if (refresh || !prev_wall_ns)
		prev_wall_ns = wall_ns;
	pthread_mutex_unlock(&thread_list_lock);

	if (actual_count)
		*actual_count = count;
	return t ? OVE_ERR_QUEUE_FULL : OVE_OK;
}
