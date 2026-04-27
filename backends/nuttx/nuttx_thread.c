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
#include <nuttx/semaphore.h>
#include <nuttx/tls.h>
#include <nuttx/mutex.h>
#include <nuttx/sched.h>
#include <nuttx/clock.h>
#include <nuttx/arch.h>
#include <sched.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <malloc.h>
/* Set thread state with tracking + trace emit (mirrors posix_thread.c). */
#define SET_STATE(t, s) do { \
	ove_trace_emit_state((uintptr_t)(t), (t)->state, (s)); \
	ove_state_track_transition(&(t)->st, (s)); \
	(t)->state = (s); \
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
		if (*pp == t) { *pp = t->next; break; }
		pp = &(*pp)->next;
	}
	nxmutex_unlock(&thread_list_lock);
}

/* Store first thread for join in start_scheduler */
static struct ove_thread *first_thread;


/* SIGUSR1 handler installed once */
static volatile int sig_handler_installed;

static int map_priority(ove_prio_t prio)
{
	/* NuttX SCHED_FIFO: higher number = higher priority */
	switch (prio) {
	case OVE_PRIO_IDLE:         return 50;
	case OVE_PRIO_LOW:          return 60;
	case OVE_PRIO_BELOW_NORMAL: return 80;
	case OVE_PRIO_NORMAL:       return 100;
	case OVE_PRIO_ABOVE_NORMAL: return 120;
	case OVE_PRIO_HIGH:         return 150;
	case OVE_PRIO_REALTIME:     return 200;
	case OVE_PRIO_CRITICAL:     return 220;
	default:                        return 100;
	}
}

static void sigusr1_handler(int sig)
{
	(void)sig;
	struct ove_thread *t = tls_get_current();
	if (t && t->suspend_inited) {
		SET_STATE(t, OVE_THREAD_STATE_SUSPENDED);
		/* Block until resumed via nxsem_post */
		while (t->state == OVE_THREAD_STATE_SUSPENDED) {
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

static int thread_start(struct ove_thread *t,
			const struct ove_thread_desc *desc)
{
	char addr_str[20];
	int pid;
	size_t stack;

	t->entry = desc->entry;
	t->arg = desc->arg;
	t->state = OVE_THREAD_STATE_READY;
	t->suspend_inited = 0;
	t->name = desc->name;   /* caller-owned string, retained for trace descriptors */
	nxsem_init(&t->done_sem, 0, 0);

	ensure_sigusr1_handler();

	stack = desc->stack_size;
	if (stack == 0) {
		stack = 2048;
	}

	snprintf(addr_str, sizeof(addr_str), "0x%lx",
		 (unsigned long)(uintptr_t)t);
	{
		char *argv_args[] = { addr_str, NULL };
		/*
		 * NuttX zero-heap reality: each ove_thread_create involves
		 * a kmm allocation that we cannot eliminate from
		 * application code:
		 *
		 *   - task_create() (current path) kmm-allocates the TCB
		 *     and stack. Switching to nxtask_init() with a caller-
		 *     supplied TCB+stack still hits sched/group/group_create.c:
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
		pid = task_create(desc->name ? desc->name : "ove_thread",
				  map_priority(desc->priority), (int)stack,
				  task_wrapper, argv_args);
		if (pid < 0) {
			nxsem_destroy(&t->done_sem);
			return OVE_ERR_NO_MEMORY;
		}
	}

	t->pid = pid;
	t->started = 1;
	_register_thread(t);

	if (first_thread == NULL) {
		first_thread = t;
	}

	return OVE_OK;
}

int ove_thread_init(ove_thread_t *handle,
			ove_thread_storage_t *storage,
			const struct ove_thread_desc *desc)
{
	if (handle == NULL || storage == NULL || desc == NULL ||
	    desc->entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* AAPCS requires 8-byte alignment at public function boundaries; a
	 * misaligned stack faults on first entry.  Sanctioned helpers in
	 * include/ove/storage.h apply aligned(8); this backstops any hand-
	 * rolled array that skips them. */
	if (desc->stack != NULL && ((uintptr_t)desc->stack & 7u) != 0u) {
		return OVE_ERR_INVALID_PARAM;
	}

	memset(storage, 0, sizeof(*storage));
	int ret = thread_start(storage, desc);
	if (ret != OVE_OK) {
		return ret;
	}

	*handle = storage;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	if (handle->started) {
		/* Resume if suspended so it can finish */
		if (handle->state == OVE_THREAD_STATE_SUSPENDED) {
			SET_STATE(handle, OVE_THREAD_STATE_READY);
			if (handle->suspend_inited) {
				nxsem_post(&handle->suspend_sem);
			}
		}
		/* Wait for thread to finish naturally (join) */
		if (handle->state != OVE_THREAD_STATE_TERMINATED) {
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
int ove_thread_create_(ove_thread_t *handle,
				const struct ove_thread_desc *desc)
{
	struct ove_thread *t;
	int ret;

	if (handle == NULL || desc == NULL || desc->entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (t == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(t, 0, sizeof(*t));

	ret = thread_start(t, desc);
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
	if (ret) return ret;

	ret = ove_thread_deinit(handle);
	OVE_BACKEND_FREE(handle);
	return ret;
}
#endif /* OVE_HEAP_THREAD */

ove_thread_t ove_thread_get_self(void)
{
	return tls_get_current();
}

void ove_thread_set_priority(ove_thread_t handle,
				      ove_prio_t prio)
{
	struct sched_param param;
	param.sched_priority = map_priority(prio);

	pid_t pid = (handle != NULL) ? handle->pid : getpid();
	sched_setparam(pid, &param);
}

void ove_thread_sleep_ms(uint32_t ms)
{
	struct ove_thread *t = tls_get_current();
	if (t) SET_STATE(t, OVE_THREAD_STATE_BLOCKED);
	usleep(ms * 1000U);
	if (t) SET_STATE(t, OVE_THREAD_STATE_RUNNING);
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
	if (handle && handle->started) {
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
	if (handle && handle->state == OVE_THREAD_STATE_SUSPENDED) {
		SET_STATE(handle, OVE_THREAD_STATE_READY);
		nxsem_post(&handle->suspend_sem);
	}
}

#ifdef CONFIG_OVE_THREAD_STATE_STATS
void ove_backend_thread_set_state(int new_state)
{
	struct ove_thread *t = tls_get_current();
	if (t) SET_STATE(t, new_state);
}
#endif

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	/* NuttX: stack usage tracking not available via POSIX API.
	 * Returns 0 (unknown) rather than an error code. */
	(void)handle;
	return 0;
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	if (handle == NULL) {
		return OVE_THREAD_STATE_TERMINATED;
	}

#ifdef __NuttX__
	/* Quick liveness check — no procfs file I/O */
	if (handle->state == OVE_THREAD_STATE_RUNNING) {
		if (kill(handle->pid, 0) != 0) {
			return OVE_THREAD_STATE_TERMINATED;
		}
	}
#endif

	return handle->state;
}

int ove_thread_get_runtime_stats(ove_thread_t handle,
					  struct ove_thread_stats *stats)
{
	(void)handle;
	stats->runtime_us = 0;
	stats->cpu_percent_x100 = 0;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats) return OVE_ERR_INVALID_PARAM;
	struct mallinfo mi = mallinfo();
	stats->total     = (size_t)mi.arena;
	stats->used      = (size_t)mi.uordblks;
	stats->free      = (size_t)mi.fordblks;
	stats->peak_used = (size_t)mi.usmblks;
	return OVE_OK;
}

struct _nuttx_list_ctx {
	struct ove_thread_info *out;
	size_t max;
	size_t count;
};

static void _nuttx_list_cb(struct tcb_s *tcb, void *arg)
{
	struct _nuttx_list_ctx *ctx = (struct _nuttx_list_ctx *)arg;
	if (ctx->count >= ctx->max)
		return;

	struct ove_thread_info *info = &ctx->out[ctx->count];

#if CONFIG_TASK_NAME_SIZE > 0
	info->name = tcb->name;
#else
	info->name = "?";
#endif

	switch (tcb->task_state) {
	case TSTATE_TASK_RUNNING:
		info->state = OVE_THREAD_STATE_RUNNING;   break;
	case TSTATE_TASK_READYTORUN:
		info->state = OVE_THREAD_STATE_READY;      break;
	case TSTATE_TASK_INACTIVE:
		info->state = OVE_THREAD_STATE_TERMINATED; break;
	default:
		/* All TSTATE_WAIT_* states are blocked */
		info->state = OVE_THREAD_STATE_BLOCKED;    break;
	}

	info->priority = (int)tcb->sched_priority;
	info->stack_size = tcb->adj_stack_size;
#ifdef CONFIG_STACK_COLORATION
	info->stack_used = up_check_tcbstack(tcb, tcb->adj_stack_size);
#else
	info->stack_used = tcb->adj_stack_size;
#endif
	info->cpu_percent_x100 = 0;

	memset(&info->state_times, 0, sizeof(info->state_times));
#ifndef CONFIG_SCHED_CPULOAD_NONE
	{
		struct cpuload_s cl;
		if (clock_cpuload(tcb->pid, &cl) == OK && cl.total > 0) {
			info->cpu_percent_x100 =
				(uint32_t)((uint64_t)cl.active * 10000U
					   / cl.total);
			/* Derive state times from tick counts (ms). */
			uint64_t run_us = (uint64_t)cl.active * 10000U;
			uint64_t tot_us = (uint64_t)cl.total * 10000U;
			info->state_times.running_us = run_us;
			info->state_times.blocked_us =
				(tot_us > run_us) ? tot_us - run_us : 0;
		}
	}
#endif

	ctx->count++;
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count,
		    size_t *actual_count)
{
	if (!out) {
		if (actual_count)
			*actual_count = 0;
		return OVE_OK;
	}

	struct _nuttx_list_ctx ctx = {
		.out = out,
		.max = max_count,
		.count = 0,
	};

	nxsched_foreach(_nuttx_list_cb, &ctx);

	if (actual_count)
		*actual_count = ctx.count;
	return OVE_OK;
}
