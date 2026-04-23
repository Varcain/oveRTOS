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
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#include <stdbool.h>
#include <string.h>

/* Set thread state with tracking + trace emit (mirrors posix/nuttx). */
#define SET_STATE(t, s) do { \
	ove_trace_emit_state((uintptr_t)(t), (t)->state, (s)); \
	ove_state_track_transition(&(t)->st, (s)); \
	(t)->state = (s); \
} while (0)

/* ── Thread registry (intrusive list) ─────────────────────────────────
 * Used by zephyr_trace.c for descriptor enumeration. The profiler is
 * ISR-driven (SysTick-style) and identifies targets via k_thread_custom_data
 * — no enumeration needed from there — so the registry exists mainly for
 * the trace swimlane. Lock is a k_mutex so it's safe from any task context
 * but not ISR; trace enumeration runs off the sim_debug pump thread. */
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

struct ove_thread *ove_zephyr_current_thread(void)
{
	/* k_thread_custom_data_get returns NULL for the idle thread /
	 * system threads; callers must handle that. */
	return (struct ove_thread *)k_thread_custom_data_get();
}

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
		if (*pp == t) { *pp = t->next; break; }
		pp = &(*pp)->next;
	}
	ove_zephyr_thread_list_unlock();
}

static int map_priority(ove_prio_t prio)
{
	/* Zephyr: lower number = higher priority (cooperative: negative) */
	switch (prio) {
	case OVE_PRIO_IDLE:         return 14;
	case OVE_PRIO_LOW:          return 12;
	case OVE_PRIO_BELOW_NORMAL: return 10;
	case OVE_PRIO_NORMAL:       return 8;
	case OVE_PRIO_ABOVE_NORMAL: return 6;
	case OVE_PRIO_HIGH:         return 4;
	case OVE_PRIO_REALTIME:     return 2;
	case OVE_PRIO_CRITICAL:     return 1;
	default:                        return 8;
	}
}

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

int ove_thread_init(ove_thread_t *handle,
			ove_thread_storage_t *storage,
			const struct ove_thread_desc *desc)
{
	k_tid_t tid;
	size_t stack_sz;

	if (handle == NULL || storage == NULL || desc == NULL ||
	    desc->entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	stack_sz = desc->stack_size;
	if (stack_sz == 0) {
		stack_sz = 8192;
	}

	/* Use desc->stack if provided (e.g. from K_THREAD_STACK_DEFINE via
	 * OVE_THREAD_DEFINE_STATIC).  Otherwise allocate via
	 * k_thread_stack_alloc for proper MPU/cache placement. */
	if (desc->stack != NULL) {
		storage->stack = (k_thread_stack_t *)desc->stack;
		storage->heap_stack = 0;
	} else {
		storage->stack = k_thread_stack_alloc(stack_sz, 0);
		if (storage->stack == NULL) {
			return OVE_ERR_NO_MEMORY;
		}
		storage->heap_stack = 1;
	}
	storage->stack_size = stack_sz;
	storage->state = OVE_THREAD_STATE_READY;
	storage->name = desc->name;  /* caller-owned; retained for trace descriptor */
	storage->next = NULL;

	tid = k_thread_create(&storage->thread, storage->stack, stack_sz,
			      thread_wrapper,
			      (void *)desc->entry, desc->arg, (void *)storage,
			      map_priority(desc->priority),
			      0, K_NO_WAIT);

	if (desc->name != NULL) {
		k_thread_name_set(tid, desc->name);
	}

	_register_thread(storage);
	*handle = storage;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

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
int ove_thread_create_(ove_thread_t *handle,
			   const struct ove_thread_desc *desc)
{
	struct ove_thread *info;
	k_thread_stack_t *stack;
	size_t stack_sz;
	k_tid_t tid;

	if (handle == NULL || desc == NULL || desc->entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	stack_sz = desc->stack_size;
	if (stack_sz == 0) {
		stack_sz = 8192;
	}

	info = OVE_BACKEND_MALLOC(sizeof(*info));
	if (info == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	stack = k_thread_stack_alloc(stack_sz, 0);
	if (stack == NULL) {
		OVE_BACKEND_FREE(info);
		return OVE_ERR_NO_MEMORY;
	}

	info->stack = stack;
	info->stack_size = stack_sz;
	info->heap_stack = 1;
	info->state = OVE_THREAD_STATE_READY;
	info->name = desc->name;
	info->next = NULL;

	tid = k_thread_create(&info->thread, stack, stack_sz,
			      thread_wrapper,
			      (void *)desc->entry, desc->arg, (void *)info,
			      map_priority(desc->priority),
			      0, K_NO_WAIT);

	if (desc->name != NULL) {
		k_thread_name_set(tid, desc->name);
	}

	_register_thread(info);
	*handle = info;
	return OVE_OK;
}

int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

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

void ove_thread_set_priority(ove_thread_t handle,
				       ove_prio_t prio)
{
	if (handle != NULL) {
		struct ove_thread *info = handle;
		k_thread_priority_set(&info->thread, map_priority(prio));
	} else {
		k_thread_priority_set(k_current_get(), map_priority(prio));
	}
}

void ove_thread_sleep_ms(uint32_t ms)
{
	struct ove_thread *t = ove_zephyr_current_thread();
	if (t) SET_STATE(t, OVE_THREAD_STATE_BLOCKED);
	k_sleep(K_MSEC(ms));
	if (t) SET_STATE(t, OVE_THREAD_STATE_RUNNING);
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

#ifdef CONFIG_OVE_THREAD_STATE_STATS
void ove_backend_thread_set_state(int new_state)
{
	struct ove_thread *t = ove_zephyr_current_thread();
	if (t) SET_STATE(t, new_state);
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

int ove_thread_get_runtime_stats(ove_thread_t handle,
					   struct ove_thread_stats *stats)
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

	stats->runtime_us = (uint64_t)(rt.execution_cycles /
				       (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U));

	k_thread_runtime_stats_t all;
	k_thread_runtime_stats_all_get(&all);
	if (all.execution_cycles > 0) {
		stats->cpu_percent_x100 =
			(uint32_t)(rt.execution_cycles * 10000ULL /
				   all.execution_cycles);
	} else {
		stats->cpu_percent_x100 = 0;
	}
	return OVE_OK;
#else
	(void)handle;
	(void)stats;
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats) return OVE_ERR_INVALID_PARAM;
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
	if (ctx->count >= ctx->max)
		return;

	struct ove_thread_info *info = &ctx->out[ctx->count];
	info->name = k_thread_name_get((k_tid_t)thread);
	if (!info->name)
		info->name = "?";
	info->state = _map_zephyr_state(thread->base.thread_state);
	info->priority = (int)k_thread_priority_get((k_tid_t)thread);

	info->cpu_percent_x100 = 0;

	/* Stack high-water mark + total */
	info->stack_used = 0;
	info->stack_size = 0;
#if defined(CONFIG_THREAD_STACK_INFO)
	info->stack_size = thread->stack_info.size;
	{
		size_t unused = 0;
		if (k_thread_stack_space_get((k_tid_t)thread, &unused) == 0)
			info->stack_used = thread->stack_info.size - unused;
	}
#endif

	/* CPU utilisation + state times */
	memset(&info->state_times, 0, sizeof(info->state_times));
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	{
		k_thread_runtime_stats_t rt;
		if (k_thread_runtime_stats_get((k_tid_t)thread, &rt) == 0) {
			k_thread_runtime_stats_t all;
			k_thread_runtime_stats_all_get(&all);
			if (all.execution_cycles > 0) {
				info->cpu_percent_x100 =
					(uint32_t)((uint64_t)rt.execution_cycles
						   * 10000U
						   / all.execution_cycles);
				/* Derive state times from cycles.
				 * Convert to us assuming 1 cycle ≈ 1 us
				 * (approximate for Zephyr timing). */
				info->state_times.running_us =
					rt.execution_cycles;
				info->state_times.blocked_us =
					(all.execution_cycles > rt.execution_cycles)
					? all.execution_cycles - rt.execution_cycles
					: 0;
			}
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

	struct _thread_list_ctx ctx = {
		.out = out,
		.max = max_count,
		.count = 0,
	};

	k_thread_foreach_unlocked(_thread_list_cb, &ctx);

	if (actual_count)
		*actual_count = ctx.count;
	return OVE_OK;
}
