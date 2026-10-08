/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS)
#include "FreeRTOS.h"

int32_t bench_get_free_heap(void)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	return -1; /* No heap in zero-heap mode */
#else
	return (int32_t)xPortGetFreeHeapSize();
#endif
}

#elif defined(CONFIG_OVE_RTOS_NUTTX)
#include <malloc.h>

int32_t bench_get_free_heap(void)
{
	struct mallinfo mi = mallinfo();
	return (int32_t)mi.fordblks;
}

#elif defined(CONFIG_OVE_RTOS_POSIX)
#include <malloc.h>

int32_t bench_get_free_heap(void)
{
	struct mallinfo2 mi = mallinfo2();
	return (int32_t)mi.fordblks;
}

#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>

extern struct k_heap _system_heap;

int32_t bench_get_free_heap(void)
{
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
	struct sys_memory_stats stats;
	if (sys_heap_runtime_stats_get(&_system_heap.heap, &stats) == 0) {
		return (int32_t)stats.free_bytes;
	}
#endif
	return -1;
}

#else
/* Unsupported backends */

int32_t bench_get_free_heap(void)
{
	return -1;
}

#endif
