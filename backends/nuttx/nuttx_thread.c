/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/thread.h"
#include "ove/storage.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h"
#include "ove_backend_common.h"
#include "ove_nuttx_priority.h"
#include "ove_nuttx_runtime.h"
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/tls.h>
#include <nuttx/mutex.h>
#include <nuttx/sched.h>
#include <nuttx/clock.h>
#include <sched.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <malloc.h>
/* Set thread state with tracking + trace emit (mirrors posix_thread.c).
 *
 * Atomic store-release on t->state matches the destroying / enumerating
 * thread's atomic acquire-load — same race that TSan flagged in POSIX
 * (sanitizers can't reach NuttX bare-metal code, so this fix is by
 * cross-backend audit rather than a triggered TSan finding).  The
 * trace-emit `from` snapshot is a relaxed load: it runs on the same
 * thread that's about to publish the new state, so no cross-thread
 * happens-before is needed for the read. */
#define SET_STATE(t, s)                                                                    \
	do {                                                                               \
		ove_trace_emit_state((uintptr_t)(t),                                       \
				     __atomic_load_n(&(t)->state, __ATOMIC_RELAXED), (s)); \
		ove_state_track_transition(&(t)->st, (s));                                 \
		__atomic_store_n(&(t)->state, (s), __ATOMIC_RELEASE);                      \
	} while (0)

/* Per-task pointer via NuttX task TLS */
static int tls_index = -1;

static void tls_ensure_init(void)
{
	if (tls_index < 0) {
		tls_index = task_tls_alloc(NULL);
	}
}

static struct ove_thread *tls_get_current(void)
{
	if (tls_index < 0) {
		return NULL;
	}
	return (struct ove_thread *)(uintptr_t)task_tls_get_value(tls_index);
}

static void tls_set_current(struct ove_thread *t)
{
	tls_ensure_init();
	task_tls_set_value(tls_index, (uintptr_t)t);
}

/* ── Thread registry (intrusive list) ─────────────────────────────────
 * Used by nuttx_trace.c for descriptor enumeration and by the profiler
 * for target enumeration. Lock is a lightweight nxmutex (kernel) so it
 * can be taken from any task context. */
struct ove_thread *ove_nuttx_thread_list_head;
static mutex_t thread_list_lock = NXMUTEX_INITIALIZER;

void ove_nuttx_thread_list_lock(void)
{
	nxmutex_lock(&thread_list_lock);
}

void ove_nuttx_thread_list_unlock(void)
{
	nxmutex_unlock(&thread_list_lock);
}

struct ove_thread *ove_nuttx_current_thread(void)
{
	return tls_get_current();
}

static void _register_thread(struct ove_thread *t)
{
	nxmutex_lock(&thread_list_lock);
	t->next = ove_nuttx_thread_list_head;
	ove_nuttx_thread_list_head = t;
	nxmutex_unlock(&thread_list_lock);
}

static void _unregister_thread(struct ove_thread *t)
{
	nxmutex_lock(&thread_list_lock);
	struct ove_thread **pp = &ove_nuttx_thread_list_head;
	while (*pp) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
		pp = &(*pp)->next;
	}
	nxmutex_unlock(&thread_list_lock);
}

/* Store first thread for join in start_scheduler */
static struct ove_thread *first_thread;

/* SIGUSR1 handler installed once */
static volatile int sig_handler_installed;

static void sigusr1_handler(int sig)
{
	(void)sig;
	struct ove_thread *t = tls_get_current();
	if (t && t->suspend_inited) {
		SET_STATE(t, OVE_THREAD_STATE_SUSPENDED);
		/* Acquire-load pairs with the resumer's release-store via
		 * SET_STATE — keeps the wake observation well-defined. */
		while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED) {
			nxsem_wait(&t->suspend_sem);
		}
	}
}

static void ensure_sigusr1_handler(void)
{
	if (!sig_handler_installed) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = sigusr1_handler;
		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR1, &sa, NULL);
		sig_handler_installed = 1;
	}
}

static int task_wrapper(int argc, char *argv[])
{
	struct ove_thread *t;

	(void)argc;
	t = (struct ove_thread *)strtoul(argv[1], NULL, 0);
	tls_set_current(t);

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	/* Init the tracker in the owning task so last_ts_us is anchored at
	 * the actual scheduling moment, not at ove_thread_init() time. */
	ove_state_track_init(&t->st, OVE_THREAD_STATE_READY);
#endif
	SET_STATE(t, OVE_THREAD_STATE_RUNNING);
	t->entry(t->arg);
	SET_STATE(t, OVE_THREAD_STATE_TERMINATED);
	nxsem_post(&t->done_sem);
	return 0;
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

static int thread_start(struct ove_thread *t, const char *name, ove_thread_fn entry, void *arg,
			ove_prio_t priority, size_t stack_size, void *stack)
{
	char addr_str[20];
	int pid;

	t->entry = entry;
	t->arg = arg;
	__atomic_store_n(&t->state, OVE_THREAD_STATE_READY, __ATOMIC_RELEASE);
	t->suspend_inited = 0;
	t->name = name; /* caller-owned string, retained for trace descriptors */
	nxsem_init(&t->done_sem, 0, 0);

	ensure_sigusr1_handler();

	if (stack_size == 0) {
		stack_size = 2048;
	}

	(void)snprintf(addr_str, sizeof(addr_str), "0x%lx", (unsigned long)(uintptr_t)t);
	{
		char *argv_args[] = {addr_str, NULL};
		/*
		 * NuttX zero-heap reality: each ove_thread_create involves
		 * a kmm allocation that we cannot eliminate from
		 * application code:
		 *
		 *   - task_create_with_stack() still kmm-allocates the TCB.
		 *     Supplying the caller's stack avoids the separate stack
		 *     allocation, but sched/group/group_create.c:
		 *     group_allocate() which kmm_zallocs sizeof(task_group_s)
		 *     for every TCB_FLAG_TTYPE_TASK.
		 *
		 *   - TCB_FLAG_TTYPE_KERNEL shares g_kthread_group and skips
		 *     the per-thread group_allocate kmm_zalloc, but the
		 *     kernel-thread launch path in CONFIG_BUILD_FLAT diverges
		 *     from the user-task path (different argv stub, no
		 *     nxtask_startup wrapper) and needs more careful setup
		 *     than a simple flag flip.  Verified empirically: kernel
		 *     threads end up in TSTATE_INVALID without running their
		 *     entry function.
		 *
		 * Bottom line: dynamic ove_thread_create on NuttX inherently
		 * touches the kernel mm region.  Apps that want the heap-
		 * lock guarantee on NuttX must structure all thread creation
		 * to happen during ove_main() (before ove_run() locks); the
		 * benchmark cannot follow that pattern because measuring
		 * dynamic create/destroy latency IS its purpose, so it
		 * bypasses ove_run() (calls ove_thread_start_scheduler()
		 * directly) on every backend.  See task #18 in the project
		 * tracker for the longer-term fix (nxtask_init with kernel-
		 * thread launch sequence).
		 */
		pid = task_create_with_stack(name ? name : "ove_thread",
					     ove_nuttx_map_priority(priority), stack,
					     (int)stack_size, task_wrapper, argv_args);
		if (pid < 0) {
			nxsem_destroy(&t->done_sem);
			return OVE_ERR_NO_MEMORY;
		}
	}

	t->pid = pid;
	__atomic_store_n(&t->started, 1, __ATOMIC_RELEASE);
	_register_thread(t);

	if (first_thread == NULL) {
		first_thread = t;
	}

	return OVE_OK;
}

int ove_thread_init(ove_thread_t *handle, ove_thread_storage_t *storage, const char *name,
		    ove_thread_fn entry, void *arg, ove_prio_t priority, size_t stack_size,
		    void *stack)
{
	if (handle == NULL || storage == NULL || entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* AAPCS requires 8-byte alignment at public function boundaries; a
	 * misaligned stack faults on first entry.  Sanctioned helpers in
	 * include/ove/storage.h apply aligned(8); this backstops any hand-
	 * rolled array that skips them. */
	if (stack != NULL && ((uintptr_t)stack & 7u) != 0u) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* NuttX can use the caller's stack, but still allocates a TCB and task
	 * group internally. The latter remains the zero-heap limitation
	 * documented in thread_start(). */

	memset(storage, 0, sizeof(*storage));
	int ret = thread_start(storage, name, entry, arg, priority, stack_size, stack);
	if (ret != OVE_OK) {
		return ret;
	}

	*handle = storage;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	if (__atomic_load_n(&handle->started, __ATOMIC_ACQUIRE)) {
		/* Resume if suspended so it can finish */
		if (__atomic_load_n(&handle->state, __ATOMIC_ACQUIRE) ==
		    OVE_THREAD_STATE_SUSPENDED) {
			SET_STATE(handle, OVE_THREAD_STATE_READY);
			if (handle->suspend_inited) {
				nxsem_post(&handle->suspend_sem);
			}
		}
		/* Wait for thread to finish naturally (join) */
		if (__atomic_load_n(&handle->state, __ATOMIC_ACQUIRE) !=
		    OVE_THREAD_STATE_TERMINATED) {
			nxsem_wait_uninterruptible(&handle->done_sem);
		}
	}

	_unregister_thread(handle);

	if (first_thread == handle) {
		first_thread = NULL;
	}

	if (handle->suspend_inited) {
		nxsem_destroy(&handle->suspend_sem);
	}
	nxsem_destroy(&handle->done_sem);
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_THREAD
int ove_thread_create(ove_thread_t *handle, const char *name, ove_thread_fn entry, void *arg,
		      ove_prio_t priority, size_t stack_size)
{
	struct ove_thread *t;
	int ret;

	if (handle == NULL || entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (t == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(t, 0, sizeof(*t));

	ret = thread_start(t, name, entry, arg, priority, stack_size, NULL);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(t);
		return ret;
	}

	*handle = t;
	return OVE_OK;
}

int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	ret = ove_thread_deinit(handle);
	OVE_BACKEND_FREE(handle);
	return ret;
}
#endif /* OVE_HEAP_THREAD */

ove_thread_t ove_thread_get_self(void)
{
	return tls_get_current();
}

void ove_thread_set_priority(ove_thread_t handle, ove_prio_t prio)
{
	struct sched_param param;
	param.sched_priority = ove_nuttx_map_priority(prio);

	pid_t pid = (handle != NULL) ? handle->pid : getpid();
	sched_setparam(pid, &param);
}

void ove_thread_sleep_ms(uint32_t ms)
{
	struct ove_thread *t = tls_get_current();
	if (t)
		SET_STATE(t, OVE_THREAD_STATE_BLOCKED);
	usleep(ms * 1000U);
	if (t)
		SET_STATE(t, OVE_THREAD_STATE_RUNNING);
}

void ove_thread_yield(void)
{
	sched_yield();
}

void ove_thread_start_scheduler(void)
{
#ifdef __NuttX__
	/* Block until first thread exits (keeps init task alive).
	 * Threads were already created by ove_thread_init() via task_create;
	 * they begin running once this task yields or blocks. */
	if (first_thread != NULL) {
		nxsem_wait_uninterruptible(&first_thread->done_sem);
	}
#endif
	/* On Linux, POSIX threads run immediately — no scheduler to start */
}

void ove_thread_suspend(ove_thread_t handle)
{
	if (handle && __atomic_load_n(&handle->started, __ATOMIC_ACQUIRE)) {
		if (!handle->suspend_inited) {
			nxsem_init(&handle->suspend_sem, 0, 0);
			handle->suspend_inited = 1;
		}
		kill(handle->pid, SIGUSR1);
		/* Wait briefly for the signal to be delivered */
		usleep(1000);
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	if (handle &&
	    __atomic_load_n(&handle->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED) {
		SET_STATE(handle, OVE_THREAD_STATE_READY);
		nxsem_post(&handle->suspend_sem);
	}
}

void ove_thread_request_stop(ove_thread_t handle)
{
	if (handle)
		__atomic_store_n(&handle->stop_requested, 1, __ATOMIC_RELEASE);
}

bool ove_thread_should_stop(ove_thread_t handle)
{
	if (!handle)
		return false;
	return __atomic_load_n(&handle->stop_requested, __ATOMIC_ACQUIRE) != 0;
}

#ifdef CONFIG_OVE_THREAD_STATE_STATS
void ove_backend_thread_set_state(int new_state)
{
	struct ove_thread *t = tls_get_current();
	if (t)
		SET_STATE(t, new_state);
}
#endif

int ove_thread_get_stack_headroom(ove_thread_t handle, size_t *headroom_bytes)
{
	if (!headroom_bytes)
		return OVE_ERR_INVALID_PARAM;
	*headroom_bytes = 0;
	if (!handle)
		return OVE_ERR_INVALID_PARAM;

	/* The public NuttX task API does not expose a safe stack high-water
	 * query for this caller-owned thread handle. */
	return OVE_ERR_NOT_SUPPORTED;
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	size_t headroom = 0;
	return ove_thread_get_stack_headroom(handle, &headroom) == OVE_OK ? headroom : 0;
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	if (handle == NULL) {
		return OVE_THREAD_STATE_TERMINATED;
	}

#ifdef __NuttX__
	/* Quick liveness check — no procfs file I/O */
	if (__atomic_load_n(&handle->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_RUNNING) {
		if (kill(handle->pid, 0) != 0) {
			return OVE_THREAD_STATE_TERMINATED;
		}
	}
#endif

	return __atomic_load_n(&handle->state, __ATOMIC_ACQUIRE);
}

int ove_thread_get_runtime_stats(ove_thread_t handle, struct ove_thread_stats *stats)
{
	(void)handle;
	stats->runtime_us = 0;
	stats->cpu_percent_x100 = 0;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats)
		return OVE_ERR_INVALID_PARAM;
	/* mallinfo() is technically not reentrant in POSIX libc, but on NuttX
	 * it is the canonical heap-stats API and the implementation grabs
	 * the kmm lock internally.  This is the ove_sys_get_mem_stats
	 * surface point — concurrent callers see consistent snapshots. */
	struct mallinfo mi = mallinfo(); /* NOLINT(concurrency-mt-unsafe) */
	stats->total = (size_t)mi.arena;
	stats->used = (size_t)mi.uordblks;
	stats->free = (size_t)mi.fordblks;
	stats->peak_used = (size_t)mi.usmblks;
	return OVE_OK;
}

struct _nuttx_list_ctx {
	struct ove_thread_info *out;
	struct _nuttx_list_snapshot *snapshot;
	size_t max;
	size_t count;
	bool overflow;
};

/* nxsched_foreach() invokes its callback while interrupts are disabled. Keep
 * that callback to a bounded scalar snapshot: stack-color scanning can touch
 * hundreds of KiB for a personality task and clock_cpuload() takes another
 * scheduler lock. A TCB cannot be pinned across the traversal, so stack_used
 * remains zero (unknown) instead of dereferencing a possibly exited task. */
struct _nuttx_list_snapshot {
	pid_t pid;
#if CONFIG_TASK_NAME_SIZE > 0
	char name[CONFIG_TASK_NAME_SIZE + 1];
#endif
};

#if defined(CONFIG_ARCH_PERF_EVENTS)

/* Exact per-task execution time. The context-switch note hook folds the elapsed
 * DWT cycles into the outgoing task. ove_thread_list() also folds the current
 * interval, so a CPU-bound task that never switches still advances on every
 * /proc refresh. Both paths run with interrupts masked while touching 64-bit
 * counters on this 32-bit core. */
#define OVE_NX_RUNTIME_MAX 32
static struct {
	pid_t pid;
	uint64_t cycles;
	bool used;
} g_nx_runtime[OVE_NX_RUNTIME_MAX];
static pid_t g_nx_runtime_current = -1;
static int g_nx_runtime_current_index = -1;
static uint32_t g_nx_runtime_last;
static uint64_t g_nx_runtime_total;
static bool g_nx_runtime_ready;
static bool g_nx_runtime_overflow;

static int _runtime_find(pid_t pid, bool create)
{
	int freeidx = -1;

	for (int i = 0; i < OVE_NX_RUNTIME_MAX; i++) {
		if (g_nx_runtime[i].used && g_nx_runtime[i].pid == pid)
			return i;
		if (!g_nx_runtime[i].used && freeidx < 0)
			freeidx = i;
	}
	if (!create || freeidx < 0) {
		if (create)
			g_nx_runtime_overflow = true;
		return -1;
	}
	g_nx_runtime[freeidx].used = true;
	g_nx_runtime[freeidx].pid = pid;
	g_nx_runtime[freeidx].cycles = 0;
	return freeidx;
}

static void _runtime_fold(uint32_t now)
{
	if (!g_nx_runtime_ready) {
		g_nx_runtime_last = now;
		g_nx_runtime_ready = true;
		return;
	}

	uint32_t delta = now - g_nx_runtime_last;
	g_nx_runtime_total += delta;
	if (g_nx_runtime_current_index >= 0)
		g_nx_runtime[g_nx_runtime_current_index].cycles += delta;
	g_nx_runtime_last = now;
}

void ove_nuttx_runtime_reset(pid_t current_pid)
{
	irqstate_t flags = enter_critical_section();
	memset(g_nx_runtime, 0, sizeof(g_nx_runtime));
	g_nx_runtime_current = current_pid;
	g_nx_runtime_current_index = current_pid >= 0 ? _runtime_find(current_pid, true) : -1;
	g_nx_runtime_total = 0;
	g_nx_runtime_last = (uint32_t)up_perf_gettime();
	g_nx_runtime_ready = true;
	g_nx_runtime_overflow = false;
	leave_critical_section(flags);
}

void ove_nuttx_runtime_start(pid_t pid)
{
	irqstate_t flags = enter_critical_section();
	int idx = _runtime_find(pid, true);
	if (idx >= 0)
		g_nx_runtime[idx].cycles = 0;
	leave_critical_section(flags);
}

void ove_nuttx_runtime_stop(pid_t pid)
{
	irqstate_t flags = enter_critical_section();
	int idx = _runtime_find(pid, false);
	if (idx >= 0) {
		if (idx == g_nx_runtime_current_index) {
			_runtime_fold((uint32_t)up_perf_gettime());
			g_nx_runtime_current = -1;
			g_nx_runtime_current_index = -1;
		}
		memset(&g_nx_runtime[idx], 0, sizeof(g_nx_runtime[idx]));
	}
	leave_critical_section(flags);
}

void ove_nuttx_runtime_switch(pid_t next_pid)
{
	/* sched_note_resume() runs from NuttX's scheduler switch path with the
	 * scheduler state protected. Keep this hot path free of a redundant
	 * nested critical section: it runs for every 1 kHz scope wakeup. */
	_runtime_fold((uint32_t)up_perf_gettime());
	g_nx_runtime_current = next_pid;
	g_nx_runtime_current_index = next_pid >= 0 ? _runtime_find(next_pid, true) : -1;
}

void ove_nuttx_runtime_snapshot(void)
{
	irqstate_t flags = enter_critical_section();
	_runtime_fold((uint32_t)up_perf_gettime());
	leave_critical_section(flags);
}

int ove_nuttx_runtime_get(pid_t pid, uint64_t *task_cycles, uint64_t *total_cycles)
{
	int ret = -1;
	irqstate_t flags = enter_critical_section();
	int idx = _runtime_find(pid, false);
	if (idx >= 0) {
		if (task_cycles)
			*task_cycles = g_nx_runtime[idx].cycles;
		ret = 0;
	}
	if (total_cycles)
		*total_cycles = g_nx_runtime_total;
	leave_critical_section(flags);
	return ret;
}

uint64_t ove_nuttx_runtime_cycles_to_us(uint64_t cycles)
{
	uint64_t freq = up_perf_getfreq();
	if (freq == 0)
		return 0;
	/* Quotient/remainder avoids overflowing cycles * 1,000,000 on long runs. */
	return (cycles / freq) * 1000000u + ((cycles % freq) * 1000000u) / freq;
}

#else

void ove_nuttx_runtime_reset(pid_t current_pid)
{
	(void)current_pid;
}

void ove_nuttx_runtime_start(pid_t pid)
{
	(void)pid;
}

void ove_nuttx_runtime_stop(pid_t pid)
{
	(void)pid;
}

void ove_nuttx_runtime_switch(pid_t next_pid)
{
	(void)next_pid;
}

void ove_nuttx_runtime_snapshot(void)
{
}

int ove_nuttx_runtime_get(pid_t pid, uint64_t *task_cycles, uint64_t *total_cycles)
{
	(void)pid;
	if (task_cycles)
		*task_cycles = 0;
	if (total_cycles)
		*total_cycles = 0;
	return -1;
}

uint64_t ove_nuttx_runtime_cycles_to_us(uint64_t cycles)
{
	(void)cycles;
	return 0;
}

#endif

/* NuttX's per-thread cpuload is an exponentially-DECAYED load average (sched_cpuload.c halves every
 * thread's tick count every ~2s), NOT a cumulative runtime. The personality's ps/top stats layer
 * charges DELTAS of running_us expecting it cumulative, so a decayed value plateaus at steady state
 * → per-proc %CPU reads 0. Rebuild a MONOTONIC cumulative running_us by integrating each thread's
 * load fraction (cpu_percent_x100) over the wall-time between ove_thread_list() calls. Keyed by pid;
 * a slot whose thread wasn't seen in a pass is reclaimed, so recreated/exited threads neither leak
 * a slot nor carry a stale total. */
#if !defined(CONFIG_ARCH_PERF_EVENTS)
#define OVE_NX_CPUINT_MAX 32
#else
#define OVE_NX_CPUINT_MAX OVE_NX_RUNTIME_MAX
#endif

static struct _nuttx_list_snapshot g_nx_list_snapshot[OVE_NX_CPUINT_MAX];
static mutex_t g_nx_list_lock = NXMUTEX_INITIALIZER;

#if !defined(CONFIG_ARCH_PERF_EVENTS)
static struct {
	pid_t pid;
	uint64_t cum_us;
	bool used;
	bool seen;
} g_nx_cpuint[OVE_NX_CPUINT_MAX];
static clock_t g_nx_cpuint_last;
static bool g_nx_cpuint_overflow;
#endif

#if defined(CONFIG_ARCH_PERF_EVENTS)
static uint32_t _runtime_percent(uint64_t part, uint64_t total)
{
	if (total == 0)
		return 0;

	/* Keep the multiply bounded on multi-month uptimes. Shifting numerator
	 * and denominator together only discards precision far below 0.01%. */
	while (total > UINT64_MAX / 10000u) {
		part >>= 1;
		total >>= 1;
	}
	return (uint32_t)(part * 10000u / total);
}
#endif

static void _nuttx_list_cb(struct tcb_s *tcb, void *arg)
{
	struct _nuttx_list_ctx *ctx = (struct _nuttx_list_ctx *)arg;
	if (ctx->count >= ctx->max) {
		ctx->overflow = true;
		return;
	}

	size_t index = ctx->count;
	struct ove_thread_info *info = &ctx->out[index];
	struct _nuttx_list_snapshot *snapshot = &ctx->snapshot[index];

#if CONFIG_TASK_NAME_SIZE > 0
	memcpy(snapshot->name, tcb->name, sizeof(snapshot->name));
	snapshot->name[sizeof(snapshot->name) - 1u] = '\0';
	info->name = snapshot->name;
#else
	info->name = "?";
#endif
	snapshot->pid = tcb->pid;
	info->identity = (uintptr_t)(uint32_t)tcb->pid;

	switch (tcb->task_state) {
	case TSTATE_TASK_RUNNING:
		info->state = OVE_THREAD_STATE_RUNNING;
		break;
	case TSTATE_TASK_READYTORUN:
		info->state = OVE_THREAD_STATE_READY;
		break;
	case TSTATE_TASK_INACTIVE:
		info->state = OVE_THREAD_STATE_TERMINATED;
		break;
	default:
		/* All TSTATE_WAIT_* states are blocked */
		info->state = OVE_THREAD_STATE_BLOCKED;
		break;
	}

	info->priority = (int)tcb->sched_priority;
	info->stack_size = tcb->adj_stack_size;
	info->stack_used = 0;
	info->cpu_percent_x100 = 0;
	memset(&info->state_times, 0, sizeof(info->state_times));

	ctx->count++;
}

static void _nuttx_list_finish(struct ove_thread_info *info,
			       const struct _nuttx_list_snapshot *snapshot, uint64_t dt_us)
{
#if defined(CONFIG_ARCH_PERF_EVENTS)
	(void)dt_us;
	uint64_t task_cycles;
	uint64_t total_cycles;
	if (ove_nuttx_runtime_get(snapshot->pid, &task_cycles, &total_cycles) == 0) {
		info->state_times.running_us = ove_nuttx_runtime_cycles_to_us(task_cycles);
		info->cpu_percent_x100 = _runtime_percent(task_cycles, total_cycles);
	}
#elif !defined(CONFIG_SCHED_CPULOAD_NONE)
	{
		struct cpuload_s cl;
		if (clock_cpuload(snapshot->pid, &cl) == OK && cl.total > 0) {
			uint32_t pct = (uint32_t)((uint64_t)cl.active * 10000U / cl.total);
			info->cpu_percent_x100 = pct;
			/* cl.active is a DECAYED average, not cumulative — integrate the load fraction
			 * into a monotonic cumulative running_us (see the g_nx_cpuint note). */
			int idx = -1, freeidx = -1;
			for (int k = 0; k < OVE_NX_CPUINT_MAX; k++) {
				if (g_nx_cpuint[k].used && g_nx_cpuint[k].pid == snapshot->pid) {
					idx = k;
					break;
				}
				if (!g_nx_cpuint[k].used && freeidx < 0)
					freeidx = k;
			}
			if (idx < 0 && freeidx >= 0) {
				idx = freeidx;
				g_nx_cpuint[idx].used = true;
				g_nx_cpuint[idx].pid = snapshot->pid;
				g_nx_cpuint[idx].cum_us = 0;
			}
			if (idx < 0)
				g_nx_cpuint_overflow = true;
			if (idx >= 0) {
				g_nx_cpuint[idx].seen = true;
				g_nx_cpuint[idx].cum_us += (uint64_t)pct * dt_us / 10000U;
				info->state_times.running_us = g_nx_cpuint[idx].cum_us;
			}
		}
	}
#endif
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count, size_t *actual_count)
{
	if (!out) {
		if (actual_count)
			*actual_count = 0;
		return OVE_OK;
	}

	nxmutex_lock(&g_nx_list_lock);
	struct _nuttx_list_ctx ctx = {
		.out = out,
		.snapshot = g_nx_list_snapshot,
		.max = max_count < OVE_NX_CPUINT_MAX ? max_count : OVE_NX_CPUINT_MAX,
		.count = 0,
	};

	/* The decayed-load fallback must integrate its samples into cumulative
	 * time. Hardware perf-counter builds already have cumulative cycles. */
#if !defined(CONFIG_ARCH_PERF_EVENTS)
	clock_t now = clock_systime_ticks();
	uint64_t dt_us =
		(g_nx_cpuint_last == 0) ? 0 : (uint64_t)(now - g_nx_cpuint_last) * USEC_PER_TICK;
	g_nx_cpuint_last = now;
	for (int k = 0; k < OVE_NX_CPUINT_MAX; k++)
		g_nx_cpuint[k].seen = false;
#else
	uint64_t dt_us = 0;
#endif

	ove_nuttx_runtime_snapshot();
	nxsched_foreach(_nuttx_list_cb, &ctx);
	for (size_t i = 0; i < ctx.count; i++)
		_nuttx_list_finish(&out[i], &g_nx_list_snapshot[i], dt_us);

#if !defined(CONFIG_ARCH_PERF_EVENTS)
	for (int k = 0; k < OVE_NX_CPUINT_MAX; k++)
		if (g_nx_cpuint[k].used && !g_nx_cpuint[k].seen)
			g_nx_cpuint[k].used = false;
#endif

	if (actual_count)
		*actual_count = ctx.count;
	nxmutex_unlock(&g_nx_list_lock);
	return (ctx.overflow
#if defined(CONFIG_ARCH_PERF_EVENTS)
		|| g_nx_runtime_overflow
#else
		|| g_nx_cpuint_overflow
#endif
		)
		       ? OVE_ERR_QUEUE_FULL
		       : OVE_OK;
}
