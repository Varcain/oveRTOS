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
#include "ove_zephyr_priority.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#include <stdbool.h>
#include <string.h>

#if defined(CONFIG_THREAD_RUNTIME_STATS)
static uint32_t runtime_percent(uint64_t part, uint64_t total)
{
	if (total == 0)
		return 0;
	while (total > UINT64_MAX / 10000u) {
		part >>= 1;
		total >>= 1;
	}
	return (uint32_t)(part * 10000u / total);
}
#endif

/* Set thread state with tracking + trace emit. With
 * CONFIG_OVE_THREAD_STATE_STATS the scheduler hooks below drive it; the
 * thread itself only marks RUNNING on entry and TERMINATED on exit.
 *
 * Atomic store-release for cross-thread visibility (list_threads /
 * SET_STATE may run from different threads); relaxed load for the
 * trace-emit `from` snapshot which always runs on the writing thread
 * itself.  Race surface is narrower in Zephyr than POSIX (destroy /
 * resume paths read Zephyr's k_thread state, not our `state` field),
 * but the macro stays consistent with the other backends. */
#ifdef CONFIG_OVE_THREAD_STATE_STATS
/* One clock read per transition, shared by the trace record and the tracker. */
#define SET_STATE(t, s)                                                                      \
	do {                                                                                 \
		uint64_t now_ = ove_state_stats_now_us();                                    \
		ove_trace_emit_state_at((uintptr_t)(t),                                      \
					__atomic_load_n(&(t)->state, __ATOMIC_RELAXED), (s), \
					now_);                                               \
		ove_state_track_transition_at(&(t)->st, (s), now_);                          \
		__atomic_store_n(&(t)->state, (s), __ATOMIC_RELEASE);                        \
	} while (0)
#else
#define SET_STATE(t, s) __atomic_store_n(&(t)->state, (s), __ATOMIC_RELEASE)
#endif

struct ove_thread *ove_zephyr_current_thread(void)
{
	/* k_thread_custom_data_get returns NULL for the idle thread /
	 * system threads; callers must handle that. */
	return (struct ove_thread *)k_thread_custom_data_get();
}

static void thread_wrapper(void *p1, void *p2, void *p3)
{
	ove_thread_fn entry = (ove_thread_fn)p1;
	void *arg = p2;
	struct ove_thread *info = (struct ove_thread *)p3;

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	/* Init the tracker in the owning task so last_ts_us is anchored at
	 * the actual scheduling moment, not at ove_thread_init() time — and
	 * before custom_data makes the thread visible to the scheduler hooks. */
	ove_state_track_init(&info->st, OVE_THREAD_STATE_READY);
#endif
	k_thread_custom_data_set(info);

	unsigned int key = irq_lock(); /* vs. the scheduler hooks */
	SET_STATE(info, OVE_THREAD_STATE_RUNNING);
	irq_unlock(key);
	entry(arg);
	key = irq_lock();
	SET_STATE(info, OVE_THREAD_STATE_TERMINATED);
	irq_unlock(key);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_thread_init(ove_thread_t *handle, ove_thread_storage_t *storage, const char *name,
		    ove_thread_fn entry, void *arg, ove_prio_t priority, size_t stack_size,
		    void *stack)
{
	k_tid_t tid;

	if (handle == NULL || storage == NULL || entry == NULL ||
	    (unsigned)priority > OVE_PRIO_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* AAPCS requires 8-byte alignment at public function boundaries; a
	 * misaligned stack faults on first entry.  Sanctioned helpers in
	 * include/ove/storage.h apply aligned(8); this backstops any hand-
	 * rolled array that skips them.  Zephyr's K_KERNEL_STACK_DEFINE
	 * already over-aligns, so caller-supplied stacks from that path
	 * pass trivially. */
	if (stack != NULL && ((uintptr_t)stack & 7u) != 0u) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (stack_size == 0) {
		stack_size = 8192;
	}

	/* Use the caller-supplied stack if provided (e.g. from
	 * K_KERNEL_STACK_DEFINE via OVE_THREAD_DEFINE_STATIC).  In zero-heap
	 * mode a NULL stack is a programmer error — there's no kernel pool
	 * to fall back to.  In heap mode fall back to k_thread_stack_alloc
	 * (requires CONFIG_DYNAMIC_THREAD). */
	if (stack != NULL) {
		storage->stack = (k_thread_stack_t *)stack;
		storage->heap_stack = 0;
	} else {
#ifdef CONFIG_OVE_ZERO_HEAP
		return OVE_ERR_NO_MEMORY;
#else
		storage->stack = k_thread_stack_alloc(stack_size, 0);
		if (storage->stack == NULL) {
			return OVE_ERR_NO_MEMORY;
		}
		storage->heap_stack = 1;
#endif
	}
	storage->stack_size = stack_size;
	storage->state = OVE_THREAD_STATE_READY;
	storage->name = name; /* caller-owned; retained for trace descriptor */
	storage->next = NULL;
	storage->stop_requested = 0;

	/* Public oveRTOS threads remain supervisor-only, so the storage helper uses
	 * Zephyr's kernel-stack layout. Protected LXP K_USER tasks are outside this
	 * API and retain their power-of-two-aligned user stacks in the port. */
	tid = k_thread_create(&storage->thread, storage->stack, stack_size, thread_wrapper,
			      (void *)entry, arg, (void *)storage,
			      ove_zephyr_map_priority(priority), 0, K_NO_WAIT);

	if (name != NULL) {
		k_thread_name_set(tid, name);
	}

	*handle = storage;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	struct ove_thread *info = handle;
	if (k_thread_join(&info->thread, K_FOREVER) != 0) {
		k_thread_abort(&info->thread);
	}
	if (info->heap_stack && info->stack != NULL) {
		k_thread_stack_free(info->stack);
		info->stack = NULL;
	}
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_THREAD
int ove_thread_create(ove_thread_t *handle, const char *name, ove_thread_fn entry, void *arg,
		      ove_prio_t priority, size_t stack_size)
{
	struct ove_thread *info;
	k_thread_stack_t *stack;
	k_tid_t tid;

	if (handle == NULL || entry == NULL || (unsigned)priority > OVE_PRIO_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (stack_size == 0) {
		stack_size = 8192;
	}

	info = OVE_BACKEND_MALLOC(sizeof(*info));
	if (info == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	stack = k_thread_stack_alloc(stack_size, 0);
	if (stack == NULL) {
		OVE_BACKEND_FREE(info);
		return OVE_ERR_NO_MEMORY;
	}

	info->stack = stack;
	info->stack_size = stack_size;
	info->heap_stack = 1;
	info->state = OVE_THREAD_STATE_READY;
	info->name = name;
	info->next = NULL;
	info->stop_requested = 0;

	tid = k_thread_create(&info->thread, stack, stack_size, thread_wrapper, (void *)entry, arg,
			      (void *)info, ove_zephyr_map_priority(priority), 0, K_NO_WAIT);

	if (name != NULL) {
		k_thread_name_set(tid, name);
	}

	*handle = info;
	return OVE_OK;
}

int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	struct ove_thread *info = handle;
	/* Try to join first (wait for thread to finish naturally),
	 * then abort if still running. */
	if (k_thread_join(&info->thread, K_FOREVER) != 0) {
		k_thread_abort(&info->thread);
	}
	k_thread_stack_free(info->stack);
	OVE_BACKEND_FREE(info);
	return OVE_OK;
}
#endif /* OVE_HEAP_THREAD */

/* Requires CONFIG_THREAD_CUSTOM_DATA=y */
ove_thread_t ove_thread_get_self(void)
{
	return k_thread_custom_data_get();
}

void ove_thread_set_priority(ove_thread_t handle, ove_prio_t prio)
{
	if ((unsigned)prio > OVE_PRIO_MAX)
		return;
	if (handle != NULL) {
		struct ove_thread *info = handle;
		k_thread_priority_set(&info->thread, ove_zephyr_map_priority(prio));
	} else {
		k_thread_priority_set(k_current_get(), ove_zephyr_map_priority(prio));
	}
}

void ove_thread_sleep_ms(uint32_t ms)
{
	k_sleep(K_MSEC(ms));
}

void ove_thread_yield(void)
{
	k_yield();
}

void ove_thread_start_scheduler(void)
{
#if defined(CONFIG_BOARD_NATIVE_SIM) || defined(OVE_QEMU_ARM)
	/* native_sim / QEMU test: scheduler already running, don't block */
#else
	/* Zephyr scheduler is already running. Block main thread forever. */
	k_sleep(K_FOREVER);
#endif
}

void ove_thread_suspend(ove_thread_t handle)
{
	if (handle != NULL) {
		struct ove_thread *info = handle;
		k_thread_suspend(&info->thread);
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	if (handle != NULL) {
		struct ove_thread *info = handle;
		k_thread_resume(&info->thread);
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
/* ── Scheduler-driven state (CONFIG_TRACING_USER hooks) ─────────────
 *
 * With state tracking on, prj.conf enables CONFIG_TRACING_USER and the
 * kernel calls these hooks on every context switch, wake-up and suspend,
 * so the tracker (and the trace stream) follow the scheduler: preemption
 * shows as READY, and waits inside the kernel, drivers or LVGL show as
 * BLOCKED, not only waits through the ove API.
 *
 * Only threads created through ove_thread_* (custom_data set in
 * thread_wrapper) are tracked. switched_out runs on PendSV entry before
 * the kernel masks interrupts, so each update runs under irq_lock()
 * against an ISR readying the same thread. Plain memory accesses only.
 */
static void sched_transition(struct k_thread *thread, int next, bool only_from_waiting)
{
	struct ove_thread *t = thread ? (struct ove_thread *)thread->custom_data : NULL;
	if (!t)
		return;

	unsigned int key = irq_lock();
	int prev = __atomic_load_n(&t->state, __ATOMIC_RELAXED);
	bool waiting = prev == OVE_THREAD_STATE_BLOCKED || prev == OVE_THREAD_STATE_SUSPENDED;
	if (prev != next && prev != OVE_THREAD_STATE_TERMINATED && (!only_from_waiting || waiting))
		SET_STATE(t, next);
	irq_unlock(key);
}

void sys_trace_thread_switched_in_user(void)
{
	sched_transition(k_current_get(), OVE_THREAD_STATE_RUNNING, false);
}

void sys_trace_thread_switched_out_user(void)
{
	struct k_thread *thread = k_current_get();
	uint8_t ks = thread->base.thread_state;
	int next;

	if (ks & _THREAD_SUSPENDED)
		next = OVE_THREAD_STATE_SUSPENDED;
	else if (ks & (_THREAD_PENDING | _THREAD_SLEEPING | _THREAD_DEAD))
		next = OVE_THREAD_STATE_BLOCKED;
	else
		next = OVE_THREAD_STATE_READY; /* preempted or yielded */
	sched_transition(thread, next, false);
}

/* Wake-up (or resume): the thread entered the run queue. Ignored unless it
 * was waiting, so a re-queue of a ready thread is not counted. */
void sys_trace_thread_sched_ready_user(struct k_thread *thread)
{
	sched_transition(thread, OVE_THREAD_STATE_READY, true);
}

/* Suspension of another thread happens without a context switch. */
void sys_trace_thread_suspend_user(struct k_thread *thread)
{
	sched_transition(thread, OVE_THREAD_STATE_SUSPENDED, false);
}
#endif

int ove_thread_get_stack_headroom(ove_thread_t handle, size_t *headroom_bytes)
{
	if (!headroom_bytes)
		return OVE_ERR_INVALID_PARAM;
	*headroom_bytes = 0;
	if (!handle)
		return OVE_ERR_INVALID_PARAM;

#if defined(CONFIG_THREAD_STACK_INFO) && defined(CONFIG_INIT_STACKS)
	struct ove_thread *info = handle;
	const uint8_t *start = (const uint8_t *)info->stack;
	size_t i;

	for (i = 0; i < info->stack_size; i++) {
		if (start[i] != 0xaaU) {
			break;
		}
	}
	*headroom_bytes = i;
	return OVE_OK;
#else
	(void)handle;
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	size_t headroom = 0;
	return ove_thread_get_stack_headroom(handle, &headroom) == OVE_OK ? headroom : 0;
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	if (handle == NULL) {
		return OVE_THREAD_STATE_UNKNOWN;
	}

	struct ove_thread *info = handle;
	uint8_t state = info->thread.base.thread_state;

	if (state & _THREAD_DEAD) {
		return OVE_THREAD_STATE_TERMINATED;
	}
	if (state & _THREAD_SUSPENDED) {
		return OVE_THREAD_STATE_SUSPENDED;
	}
	if (state & _THREAD_PENDING) {
		return OVE_THREAD_STATE_BLOCKED;
	}
	if (state & _THREAD_QUEUED) {
		return OVE_THREAD_STATE_READY;
	}
	return OVE_THREAD_STATE_RUNNING;
}

int ove_thread_get_runtime_stats(ove_thread_t handle, struct ove_thread_stats *stats)
{
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	if (handle == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_thread *info = handle;
	k_thread_runtime_stats_t rt;

	int ret = k_thread_runtime_stats_get(&info->thread, &rt);
	if (ret != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	stats->runtime_us = k_cyc_to_us_floor64(rt.execution_cycles);

	k_thread_runtime_stats_t all;
	k_thread_runtime_stats_all_get(&all);
	stats->cpu_percent_x100 = runtime_percent(rt.execution_cycles, all.execution_cycles);
	return OVE_OK;
#else
	(void)handle;
	(void)stats;
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats)
		return OVE_ERR_INVALID_PARAM;
	memset(stats, 0, sizeof(*stats));
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	extern struct k_heap _system_heap;
	struct sys_memory_stats hstats;
	sys_heap_runtime_stats_get(&_system_heap.heap, &hstats);
	stats->free = hstats.free_bytes;
	stats->used = hstats.allocated_bytes;
	stats->total = hstats.free_bytes + hstats.allocated_bytes;
	stats->peak_used = hstats.max_allocated_bytes;
#endif
	return OVE_OK;
}

struct _thread_list_ctx {
	struct ove_thread_info *out;
	size_t max;
	size_t count;
	uint64_t total_cycles;
	bool overflow;
};

static ove_thread_state_t _map_zephyr_state(uint8_t state)
{
	if (state & _THREAD_DEAD)
		return OVE_THREAD_STATE_TERMINATED;
	if (state & _THREAD_SUSPENDED)
		return OVE_THREAD_STATE_SUSPENDED;
	if (state & _THREAD_PENDING)
		return OVE_THREAD_STATE_BLOCKED;
	if (state & _THREAD_QUEUED)
		return OVE_THREAD_STATE_READY;
	return OVE_THREAD_STATE_RUNNING;
}

static void _thread_list_cb(const struct k_thread *thread, void *user_data)
{
	struct _thread_list_ctx *ctx = (struct _thread_list_ctx *)user_data;
	if (ctx->count >= ctx->max) {
		ctx->overflow = true;
		return;
	}

	struct ove_thread_info *info = &ctx->out[ctx->count];
	info->name = k_thread_name_get((k_tid_t)thread);
	if (!info->name)
		info->name = "?";
	info->identity = (uintptr_t)thread;
	info->state = _map_zephyr_state(thread->base.thread_state);
	info->native_priority = k_thread_priority_get((k_tid_t)thread);
	info->priority = (int)ove_zephyr_unmap_priority(info->native_priority);

	info->cpu_percent_x100 = 0;
	info->valid_fields = 0;

	/* Keep the monitor-locked callback bounded. A full stack-fill scan can
	 * touch hundreds of KiB and is not safe inside this snapshot. */
	info->stack_used = 0;
	info->stack_size = 0;
#if defined(CONFIG_THREAD_STACK_INFO)
	info->stack_size = thread->stack_info.size;
	info->valid_fields |= OVE_THREAD_INFO_VALID_STACK_SIZE;
#endif

	/* CPU utilisation + state times */
	memset(&info->state_times, 0, sizeof(info->state_times));
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	{
		k_thread_runtime_stats_t rt;
		if (k_thread_runtime_stats_get((k_tid_t)thread, &rt) == 0) {
			if (ctx->total_cycles > 0) {
				info->cpu_percent_x100 =
					runtime_percent(rt.execution_cycles, ctx->total_cycles);
				/* Zephyr accounts cumulative execution in hardware cycles.
				 * Preserve that precision and convert to the microsecond unit
				 * promised by ove_thread_info/LXP rather than treating one
				 * core cycle as one microsecond. */
				info->state_times.running_us =
					k_cyc_to_us_floor64(rt.execution_cycles);
				info->valid_fields |= OVE_THREAD_INFO_VALID_CPU_PERCENT |
						      OVE_THREAD_INFO_VALID_RUNNING_TIME;
			}
		}
	}
#endif

	ctx->count++;
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count, size_t *actual_count)
{
	if (!out) {
		if (actual_count)
			*actual_count = 0;
		return OVE_OK;
	}

	struct _thread_list_ctx ctx = {
		.out = out,
		.max = max_count,
		.count = 0,
		.total_cycles = 0,
	};

#if defined(CONFIG_THREAD_RUNTIME_STATS)
	k_thread_runtime_stats_t all;
	k_thread_runtime_stats_all_get(&all);
	ctx.total_cycles = all.execution_cycles;
#endif
	/* The locked form prevents a thread object from being aborted and reused
	 * while its identity and counters are copied. */
	k_thread_foreach(_thread_list_cb, &ctx);

	if (actual_count)
		*actual_count = ctx.count;
	return ctx.overflow ? OVE_ERR_QUEUE_FULL : OVE_OK;
}
