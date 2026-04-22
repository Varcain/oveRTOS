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
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

static void freertos_thread_wrapper(void *param)
{
	struct ove_thread *s = (struct ove_thread *)param;
	void (*entry)(void *) = s->entry;
	void *arg = s->arg;
	entry(arg);
	xSemaphoreGive(s->done_sem);
	vTaskSuspend(NULL);
}

static UBaseType_t map_priority(ove_prio_t prio)
{
	/* Direct mapping: ove priority value + tskIDLE_PRIORITY */
	return tskIDLE_PRIORITY + (UBaseType_t)prio;
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_thread_init(ove_thread_t *handle,
			ove_thread_storage_t *storage,
			const struct ove_thread_desc *desc)
{
	if (handle == NULL || storage == NULL || desc == NULL ||
	    desc->entry == NULL || desc->stack == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->done_sem = xSemaphoreCreateBinaryStatic(&storage->static_done_sem);
	storage->entry = desc->entry;
	storage->arg = desc->arg;

	ove_state_track_init(&storage->st, OVE_THREAD_STATE_READY);

	uint32_t stack_depth = desc->stack_size / sizeof(StackType_t);
	if (stack_depth < configMINIMAL_STACK_SIZE)
		stack_depth = configMINIMAL_STACK_SIZE;

	storage->task = xTaskCreateStatic(
		freertos_thread_wrapper, desc->name,
		stack_depth, storage,
		map_priority(desc->priority),
		(StackType_t *)desc->stack,
		&storage->static_task);

	vTaskSetApplicationTaskTag(storage->task, (TaskHookFunction_t)storage);
	*handle = storage;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	xSemaphoreTake(handle->done_sem, portMAX_DELAY);
	vTaskDelete(handle->task);
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_THREAD
int ove_thread_create_(ove_thread_t *handle,
			   const struct ove_thread_desc *desc)
{
	if (handle == NULL || desc == NULL || desc->entry == NULL)
		return OVE_ERR_INVALID_PARAM;

	struct ove_thread *wrapper = OVE_BACKEND_MALLOC(sizeof(*wrapper));
	if (wrapper == NULL)
		return OVE_ERR_NO_MEMORY;

	wrapper->done_sem = xSemaphoreCreateBinaryStatic(&wrapper->static_done_sem);
	wrapper->entry = desc->entry;
	wrapper->arg = desc->arg;

	ove_state_track_init(&wrapper->st, OVE_THREAD_STATE_READY);

	uint32_t stack_depth = desc->stack_size / sizeof(StackType_t);
	if (stack_depth < configMINIMAL_STACK_SIZE)
		stack_depth = configMINIMAL_STACK_SIZE;

	BaseType_t ret = xTaskCreate(
		freertos_thread_wrapper, desc->name,
		stack_depth, wrapper,
		map_priority(desc->priority), &wrapper->task);
	if (ret != pdPASS) {
		OVE_BACKEND_FREE(wrapper);
		return OVE_ERR_NO_MEMORY;
	}

	vTaskSetApplicationTaskTag(wrapper->task, (TaskHookFunction_t)wrapper);
	*handle = wrapper;
	return OVE_OK;
}

int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	xSemaphoreTake(handle->done_sem, portMAX_DELAY);
	vTaskDelete(handle->task);
	OVE_BACKEND_FREE(handle);
	return OVE_OK;
}
#endif /* OVE_HEAP_THREAD */

ove_thread_t ove_thread_get_self(void)
{
	return (ove_thread_t)xTaskGetApplicationTaskTag(
		xTaskGetCurrentTaskHandle());
}

void ove_thread_set_priority(ove_thread_t handle,
					 ove_prio_t prio)
{
	TaskHandle_t task = (handle != NULL) ? handle->task : NULL;
	vTaskPrioritySet(task, map_priority(prio));
}

void ove_thread_sleep_ms(uint32_t ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

void ove_thread_yield(void)
{
	taskYIELD();
}

void ove_thread_start_scheduler(void)
{
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
		return;
	}
	vTaskStartScheduler();
}

void ove_thread_suspend(ove_thread_t handle)
{
	vTaskSuspend(handle->task);
}

void ove_thread_resume(ove_thread_t handle)
{
	vTaskResume(handle->task);
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	UBaseType_t hwm = uxTaskGetStackHighWaterMark(handle->task);
	return (size_t)(hwm * sizeof(StackType_t));
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	eTaskState state = eTaskGetState(handle->task);

	switch (state) {
	case eRunning:   return OVE_THREAD_STATE_RUNNING;
	case eReady:     return OVE_THREAD_STATE_READY;
	case eBlocked:   return OVE_THREAD_STATE_BLOCKED;
	case eSuspended: return OVE_THREAD_STATE_SUSPENDED;
	case eDeleted:   return OVE_THREAD_STATE_TERMINATED;
	default:         return OVE_THREAD_STATE_UNKNOWN;
	}
}

int ove_thread_get_runtime_stats(ove_thread_t handle,
					     struct ove_thread_stats *stats)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
	TaskStatus_t task_status;
	vTaskGetInfo(handle->task, &task_status, pdFALSE, eInvalid);

	stats->runtime_us = (uint64_t)task_status.ulRunTimeCounter;

	uint32_t total = portGET_RUN_TIME_COUNTER_VALUE();
	if (total > 0) {
		stats->cpu_percent_x100 =
			(uint32_t)((uint64_t)task_status.ulRunTimeCounter *
				   10000ULL / total);
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
#if configSUPPORT_DYNAMIC_ALLOCATION
	stats->total     = configTOTAL_HEAP_SIZE;
	stats->free      = xPortGetFreeHeapSize();
	stats->used      = stats->total - stats->free;
	stats->peak_used = stats->total - xPortGetMinimumEverFreeHeapSize();
#else
	stats->total     = 0;
	stats->free      = 0;
	stats->used      = 0;
	stats->peak_used = 0;
#endif
	return OVE_OK;
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count,
		    size_t *actual_count)
{
#if configUSE_TRACE_FACILITY
	if (!out) {
		if (actual_count)
			*actual_count = (size_t)uxTaskGetNumberOfTasks();
		return OVE_OK;
	}

	UBaseType_t count = uxTaskGetNumberOfTasks();
	if (count > (UBaseType_t)max_count)
		count = (UBaseType_t)max_count;

	TaskStatus_t *tasks = pvPortMalloc(count * sizeof(TaskStatus_t));
	if (!tasks) return OVE_ERR_NO_MEMORY;

	uint32_t total_runtime = 0;
	UBaseType_t filled = uxTaskGetSystemState(tasks, count,
						  &total_runtime);
	for (UBaseType_t i = 0; i < filled; i++) {
		out[i].name       = tasks[i].pcTaskName;
		out[i].priority   = (int)tasks[i].uxCurrentPriority;
		{
			size_t min_free = (size_t)tasks[i].usStackHighWaterMark
					  * sizeof(StackType_t);
#if (configRECORD_STACK_HIGH_ADDRESS == 1)
			size_t total = (size_t)(
				(uintptr_t)tasks[i].pxEndOfStack
				- (uintptr_t)tasks[i].pxStackBase)
				+ sizeof(StackType_t);
			out[i].stack_size = total;
			out[i].stack_used = total - min_free;
#else
			out[i].stack_size = 0;
			out[i].stack_used = min_free; /* min-free only */
#endif
		}
		switch (tasks[i].eCurrentState) {
		case eRunning:   out[i].state = OVE_THREAD_STATE_RUNNING;   break;
		case eReady:     out[i].state = OVE_THREAD_STATE_READY;     break;
		case eBlocked:   out[i].state = OVE_THREAD_STATE_BLOCKED;   break;
		case eSuspended: out[i].state = OVE_THREAD_STATE_SUSPENDED; break;
		default:         out[i].state = OVE_THREAD_STATE_UNKNOWN;   break;
		}
		/* CPU utilisation: task_runtime / total_runtime * 10000 */
		if (total_runtime > 0)
			out[i].cpu_percent_x100 =
				(uint32_t)((uint64_t)tasks[i].ulRunTimeCounter
					   * 10000U / total_runtime);
		else
			out[i].cpu_percent_x100 = 0;

		/* State times: derive from runtime counter (ms).
		 * running = ulRunTimeCounter, blocked ≈ total - running. */
		uint64_t run_us = (uint64_t)tasks[i].ulRunTimeCounter * 1000U;
		uint64_t tot_us = (uint64_t)total_runtime * 1000U;
		out[i].state_times.running_us   = run_us;
		out[i].state_times.ready_us     = 0;
		out[i].state_times.blocked_us   = (tot_us > run_us) ? tot_us - run_us : 0;
		out[i].state_times.suspended_us = 0;
	}
	if (actual_count)
		*actual_count = (size_t)filled;

	vPortFree(tasks);
	return OVE_OK;
#else
	(void)out;
	(void)max_count;
	(void)actual_count;
	return OVE_ERR_NOT_SUPPORTED;
#endif /* configUSE_TRACE_FACILITY */
}
