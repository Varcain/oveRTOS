/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/thread.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include <zephyr/kernel.h>

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
	entry(arg);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_thread_init(ove_thread_t *handle,
			ove_thread_storage_t *storage,
			const struct ove_thread_desc *desc)
{
	k_tid_t tid;
	size_t stack_sz;

	if (handle == NULL || storage == NULL || desc == NULL ||
	    desc->entry == NULL || desc->stack == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	stack_sz = desc->stack_size;
	if (stack_sz == 0) {
		stack_sz = 8192;
	}

	storage->stack = (k_thread_stack_t *)desc->stack;
	storage->stack_size = stack_sz;

	tid = k_thread_create(&storage->thread, storage->stack, stack_sz,
			      thread_wrapper,
			      (void *)desc->entry, desc->arg, (void *)storage,
			      map_priority(desc->priority),
			      0, K_NO_WAIT);

	if (desc->name != NULL) {
		k_thread_name_set(tid, desc->name);
	}

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
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_THREAD
int ove_thread_create(ove_thread_t *handle,
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

	tid = k_thread_create(&info->thread, stack, stack_sz,
			      thread_wrapper,
			      (void *)desc->entry, desc->arg, (void *)info,
			      map_priority(desc->priority),
			      0, K_NO_WAIT);

	if (desc->name != NULL) {
		k_thread_name_set(tid, desc->name);
	}

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
