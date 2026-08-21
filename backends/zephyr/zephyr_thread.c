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

/* Set thread state with tracking + trace emit (mirrors posix/nuttx).
 *
 * Atomic store-release for cross-thread visibility (list_threads /
 * SET_STATE may run from different threads); relaxed load for the
 * trace-emit `from` snapshot which always runs on the writing thread
 * itself.  Race surface is narrower in Zephyr than POSIX (destroy /
 * resume paths read Zephyr's k_thread state, not our `state` field),
 * but the macro stays consistent with the other backends. */
#define SET_STATE(t, s)                                                                    \
	do {                                                                               \
		ove_trace_emit_state((uintptr_t)(t),                                       \
				     __atomic_load_n(&(t)->state, __ATOMIC_RELAXED), (s)); \
		ove_state_track_transition(&(t)->st, (s));                                 \
		__atomic_store_n(&(t)->state, (s), __ATOMIC_RELEASE);                      \
	} while (0)

/* ── Thread registry (intrusive list) ─────────────────────────────────
 * Used by zephyr_trace.c for descriptor enumeration. The profiler is
 * ISR-driven (SysTick-style) and identifies targets via k_thread_custom_data
 * — no enumeration needed from there — so the registry exists mainly for
 * the trace swimlane. Lock is a k_mutex so it's safe from any task context
 * but not ISR; trace enumeration runs off the sim_debug pump thread.
 *
 * Gated on CONFIG_OVE_TRACE_STREAM — the same guard the sole consumer,
 * ove_backend_trace_list_threads() in zephyr_trace.c, compiles under. With
 * trace off the registry is pure overhead (a global k_mutex taken twice on
 * every thread create/destroy), so it and _register/_unregister compile to
 * nothing. struct ove_thread keeps its `next` field either way so the
 * storage size (size probes / test_storage_bounds) is unchanged. */
#ifdef CONFIG_OVE_TRACE_STREAM
struct ove_thread *ove_zephyr_thread_list_head;
static struct k_mutex thread_list_lock;
static bool thread_list_lock_initialised;

static void ensure_list_lock(void)
{
	if (!thread_list_lock_initialised) {
		k_mutex_init(&thread_list_lock);
		thread_list_lock_initialised = true;
	}
}

void ove_zephyr_thread_list_lock(void)
{
	ensure_list_lock();
	k_mutex_lock(&thread_list_lock, K_FOREVER);
}

void ove_zephyr_thread_list_unlock(void)
{
	k_mutex_unlock(&thread_list_lock);
}
#endif /* CONFIG_OVE_TRACE_STREAM */

struct ove_thread *ove_zephyr_current_thread(void)
{
	/* k_thread_custom_data_get returns NULL for the idle thread /
	 * system threads; callers must handle that. */
	return (struct ove_thread *)k_thread_custom_data_get();
}

#ifdef CONFIG_OVE_TRACE_STREAM
static void _register_thread(struct ove_thread *t)
{
	ove_zephyr_thread_list_lock();
	t->next = ove_zephyr_thread_list_head;
	ove_zephyr_thread_list_head = t;
	ove_zephyr_thread_list_unlock();
}

static void _unregister_thread(struct ove_thread *t)
{
	ove_zephyr_thread_list_lock();
	struct ove_thread **pp = &ove_zephyr_thread_list_head;
	while (*pp) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
		pp = &(*pp)->next;
	}
	ove_zephyr_thread_list_unlock();
}
#else
/* Trace off: the registry is compiled out (see comment above); these
 * no-ops keep the create/destroy/init/deinit call sites unchanged. */
static inline void _register_thread(struct ove_thread *t)
{
	(void)t;
}
static inline void _unregister_thread(struct ove_thread *t)
{
	(void)t;
}
#endif /* CONFIG_OVE_TRACE_STREAM */

static void thread_wrapper(void *p1, void *p2, void *p3)
{
	ove_thread_fn entry = (ove_thread_fn)p1;
	void *arg = p2;
	struct ove_thread *info = (struct ove_thread *)p3;
	k_thread_custom_data_set(info);

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	/* Init the tracker in the owning task so last_ts_us is anchored at
	 * the actual scheduling moment, not at ove_thread_init() time. */
	ove_state_track_init(&info->st, OVE_THREAD_STATE_READY);
#endif
	SET_STATE(info, OVE_THREAD_STATE_RUNNING);
	entry(arg);
	SET_STATE(info, OVE_THREAD_STATE_TERMINATED);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_thread_init(ove_thread_t *handle, ove_thread_storage_t *storage, const char *name,
		    ove_thread_fn entry, void *arg, ove_prio_t priority, size_t stack_size,
		    void *stack)
{
	k_tid_t tid;

	if (handle == NULL || storage == NULL || entry == NULL) {
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

	_register_thread(storage);
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
	_unregister_thread(info);
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

	if (handle == NULL || entry == NULL) {
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

	_register_thread(info);
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
	_unregister_thread(info);
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
	if (handle != NULL) {
		struct ove_thread *info = handle;
		k_thread_priority_set(&info->thread, ove_zephyr_map_priority(prio));
	} else {
		k_thread_priority_set(k_current_get(), ove_zephyr_map_priority(prio));
	}
}

void ove_thread_sleep_ms(uint32_t ms)
{
	struct ove_thread *t = ove_zephyr_current_thread();
	if (t)
		SET_STATE(t, OVE_THREAD_STATE_BLOCKED);
	k_sleep(K_MSEC(ms));
	if (t)
		SET_STATE(t, OVE_THREAD_STATE_RUNNING);
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
		SET_STATE(info, OVE_THREAD_STATE_SUSPENDED);
		k_thread_suspend(&info->thread);
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	if (handle != NULL) {
		struct ove_thread *info = handle;
		SET_STATE(info, OVE_THREAD_STATE_READY);
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
void ove_backend_thread_set_state(int new_state)
{
	struct ove_thread *t = ove_zephyr_current_thread();
	if (t)
		SET_STATE(t, new_state);
}
#endif

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
#if defined(CONFIG_THREAD_STACK_INFO) && defined(CONFIG_INIT_STACKS)
	if (handle != NULL) {
		struct ove_thread *info = handle;
		const uint8_t *start = (const uint8_t *)info->stack;
		size_t i;

		for (i = 0; i < info->stack_size; i++) {
			if (start[i] != 0xaaU) {
				break;
			}
		}
		return i;
	}
#else
	(void)handle;
#endif
	return 0;
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
	info->priority = (int)k_thread_priority_get((k_tid_t)thread);

	info->cpu_percent_x100 = 0;

	/* Keep the monitor-locked callback bounded. A full stack-fill scan can
	 * touch hundreds of KiB and is not safe inside this snapshot. */
	info->stack_used = 0;
	info->stack_size = 0;
#if defined(CONFIG_THREAD_STACK_INFO)
	info->stack_size = thread->stack_info.size;
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
				info->state_times.blocked_us =
					(ctx->total_cycles > rt.execution_cycles)
						? k_cyc_to_us_floor64(ctx->total_cycles -
								      rt.execution_cycles)
						: 0;
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
