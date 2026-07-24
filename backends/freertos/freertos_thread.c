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
#include "ove_freertos_lnx_metrics.h"
#include "ove_freertos_priority.h"
#include "ove_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdatomic.h>

/* Portable Cortex-M memory barrier.  CMSIS exposes `__DMB()` via
 * `cmsis_compiler.h`, but the QEMU MPS2-AN500 FreeRTOS test build
 * doesn't pull CMSIS in, so use the standard C11 atomic_thread_fence
 * (GCC emits `dmb sy` on ARMv7-M) for portability. */
#define OVE_DMB() atomic_thread_fence(memory_order_seq_cst)

#if (configGENERATE_RUN_TIME_STATS == 1) || (configUSE_TRACE_FACILITY == 1)
static uint32_t runtime_percent(configRUN_TIME_COUNTER_TYPE part,
				configRUN_TIME_COUNTER_TYPE total)
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

static void freertos_thread_wrapper(void *param)
{
	struct ove_thread *s = (struct ove_thread *)param;
	void (*entry)(void *) = s->entry;
	void *arg = s->arg;
	entry(arg);
	/* Dekker-style join handshake (paired with wait_for_worker_exit()):
	 *   1. publish exited = 1
	 *   2. memory barrier so the destroyer's read of `destroyer` (below)
	 *      can't be reordered before the publish
	 *   3. read destroyer; if non-null, the destroyer is or will be
	 *      blocked on its own notification slot — wake it
	 *
	 * The barrier on this side, paired with the destroyer's
	 * publish-then-DMB-then-read of `exited`, guarantees that at least
	 * one of {worker sees destroyer, destroyer sees exited} is true.
	 * That is enough to ensure no deadlock: if worker doesn't notify
	 * (because destroyer hadn't published yet), destroyer's read of
	 * exited will see 1 and skip the blocking ulTaskNotifyTake. */
	s->exited = 1u;
	OVE_DMB();
	if (s->destroyer != NULL) {
		xTaskNotifyGive(s->destroyer);
	}
	vTaskSuspend(NULL);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_thread_init(ove_thread_t *handle, ove_thread_storage_t *storage, const char *name,
		    ove_thread_fn entry, void *arg, ove_prio_t priority, size_t stack_size,
		    void *stack)
{
	if (handle == NULL || storage == NULL || entry == NULL || stack == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* AAPCS requires the stack pointer to be 8-byte aligned at public
	 * function boundaries; a misaligned stack faults immediately on the
	 * first thread entry.  The sanctioned helper macros
	 * (OVE_THREAD_STACK_DEFINE_, OVE_THREAD_STACK_MEMBER_,
	 * OVE_THREAD_STACK_BLOCK_STATIC_) all apply aligned(8); this runtime
	 * check backstops hand-rolled stack arrays that bypass those. */
	if (((uintptr_t)stack & 7u) != 0u) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->entry = entry;
	storage->arg = arg;
	storage->destroyer = NULL;
	storage->exited = 0u;
	storage->stop_requested = 0;

	ove_state_track_init(&storage->st, OVE_THREAD_STATE_READY);

	uint32_t stack_depth = stack_size / sizeof(StackType_t);
	if (stack_depth < configMINIMAL_STACK_SIZE)
		stack_depth = configMINIMAL_STACK_SIZE;

	storage->task = xTaskCreateStatic(freertos_thread_wrapper, name, stack_depth, storage,
					  ove_freertos_map_priority(priority), (StackType_t *)stack,
					  &storage->static_task);

	vTaskSetApplicationTaskTag(storage->task, (TaskHookFunction_t)storage);
	*handle = storage;
	return OVE_OK;
}

/* Wait for the worker thread to finish its entry().  Dekker-style
 * handshake (worker side in freertos_thread_wrapper):
 *
 *   1. publish destroyer = ourself
 *   2. memory barrier
 *   3. read worker's `exited` flag
 *      - 1 → worker is past its entry().  It MAY have notified us (if
 *            it observed our destroyer publish) — drain any stale
 *            notification and return.
 *      - 0 → worker is still running entry().  By the barrier rule it
 *            will observe our destroyer write and notify us when it
 *            transitions out of entry().  Block on the notification.
 *
 * Replaces the earlier xSemaphoreCreateBinaryStatic / Take-Give pair
 * (~3 µs round trip + a kernel object) with a barrier + a one-word
 * read on the typical (worker-already-done) path. */
static void wait_for_worker_exit(ove_thread_t handle)
{
	handle->destroyer = xTaskGetCurrentTaskHandle();
	OVE_DMB();
	if (handle->exited) {
		(void)ulTaskNotifyTake(pdTRUE, 0);
	} else {
		(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	}
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	wait_for_worker_exit(handle);
	vTaskDelete(handle->task);
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_THREAD
int ove_thread_create(ove_thread_t *handle, const char *name, ove_thread_fn entry, void *arg,
		      ove_prio_t priority, size_t stack_size)
{
	if (handle == NULL || entry == NULL)
		return OVE_ERR_INVALID_PARAM;

	uint32_t stack_depth = stack_size / sizeof(StackType_t);
	if (stack_depth < configMINIMAL_STACK_SIZE)
		stack_depth = configMINIMAL_STACK_SIZE;

	size_t stack_bytes = (size_t)stack_depth * sizeof(StackType_t);
#if (configSTACK_ALLOCATION_FROM_SEPARATE_HEAP == 1)
	/* Keep the wrapper/TCB bookkeeping in the general heap, but obtain the actual task stack from
	 * the port's Normal-memory stack heap.  STM32 Linux builds deliberately keep the general heap
	 * in Device-mapped SDRAM, which is not safe for exception stacking. */
	struct ove_thread *wrapper = OVE_BACKEND_MALLOC(sizeof(*wrapper));
#else
	/* Single allocation for the wrapper struct and flexible-array task stack on ports where both
	 * use the same memory attributes. */
	struct ove_thread *wrapper = OVE_BACKEND_MALLOC(sizeof(*wrapper) + stack_bytes);
#endif
	if (wrapper == NULL)
		return OVE_ERR_NO_MEMORY;

#if (configSTACK_ALLOCATION_FROM_SEPARATE_HEAP == 1)
	wrapper->stack = pvPortMallocStack(stack_bytes);
	if (wrapper->stack == NULL) {
		OVE_BACKEND_FREE(wrapper);
		return OVE_ERR_NO_MEMORY;
	}
#endif

	wrapper->entry = entry;
	wrapper->arg = arg;
	wrapper->destroyer = NULL;
	wrapper->exited = 0u;
	wrapper->stop_requested = 0;

	ove_state_track_init(&wrapper->st, OVE_THREAD_STATE_READY);

	wrapper->task = xTaskCreateStatic(freertos_thread_wrapper, name, stack_depth, wrapper,
					  ove_freertos_map_priority(priority), wrapper->stack,
					  &wrapper->static_task);
	if (wrapper->task == NULL) {
#if (configSTACK_ALLOCATION_FROM_SEPARATE_HEAP == 1)
		vPortFreeStack(wrapper->stack);
#endif
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
	if (ret)
		return ret;

	wait_for_worker_exit(handle);
	vTaskDelete(handle->task);
#if (configSTACK_ALLOCATION_FROM_SEPARATE_HEAP == 1)
	vPortFreeStack(handle->stack);
#endif
	OVE_BACKEND_FREE(handle);
	return OVE_OK;
}
#endif /* OVE_HEAP_THREAD */

ove_thread_t ove_thread_get_self(void)
{
	return (ove_thread_t)xTaskGetApplicationTaskTag(xTaskGetCurrentTaskHandle());
}

void ove_thread_set_priority(ove_thread_t handle, ove_prio_t prio)
{
	TaskHandle_t task = (handle != NULL) ? handle->task : NULL;
	vTaskPrioritySet(task, ove_freertos_priority_value(prio));
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

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	UBaseType_t hwm = uxTaskGetStackHighWaterMark(handle->task);
	return (size_t)(hwm * sizeof(StackType_t));
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	eTaskState state = eTaskGetState(handle->task);

	switch (state) {
	case eRunning:
		return OVE_THREAD_STATE_RUNNING;
	case eReady:
		return OVE_THREAD_STATE_READY;
	case eBlocked:
		return OVE_THREAD_STATE_BLOCKED;
	case eSuspended:
		return OVE_THREAD_STATE_SUSPENDED;
	case eDeleted:
		return OVE_THREAD_STATE_TERMINATED;
	default:
		return OVE_THREAD_STATE_UNKNOWN;
	}
}

int ove_thread_get_runtime_stats(ove_thread_t handle, struct ove_thread_stats *stats)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
	TaskStatus_t task_status;
	vTaskGetInfo(handle->task, &task_status, pdFALSE, eInvalid);

	configRUN_TIME_COUNTER_TYPE total = portGET_RUN_TIME_COUNTER_VALUE();
#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
	stats->runtime_us = (uint64_t)task_status.ulRunTimeCounter * 1000u;
#else
	stats->runtime_us = (uint64_t)task_status.ulRunTimeCounter / (SystemCoreClock / 1000000u);
#endif

	stats->cpu_percent_x100 = runtime_percent(task_status.ulRunTimeCounter, total);
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
#if configSUPPORT_DYNAMIC_ALLOCATION
	stats->total = configTOTAL_HEAP_SIZE;
	stats->free = xPortGetFreeHeapSize();
	stats->used = stats->total - stats->free;
	stats->peak_used = stats->total - xPortGetMinimumEverFreeHeapSize();
#else
	stats->total = 0;
	stats->free = 0;
	stats->used = 0;
	stats->peak_used = 0;
#endif
	return OVE_OK;
}

#define OVE_FRT_TASK_REGISTRY_MAX 24

#if configUSE_TRACE_FACILITY
/*
 * uxTaskGetSystemState() suspends scheduling while it traverses every kernel
 * list and scans every task's untouched stack fill. On the STM32 personality
 * that took 368-532 us every 200 ms, directly creating the dispatch tail.
 *
 * FreeRTOS invokes traceTASK_CREATE/DELETE while its task-list critical section
 * is held. Maintain the handles and immutable names there, then take a bounded
 * no-stack-scan snapshot below. The registry has headroom above LXP's sixteen
 * visible kernel-thread entries so temporary service tasks do not overflow it.
 */
struct frt_task_registry_entry {
	TaskHandle_t handle;
	char name[configMAX_TASK_NAME_LEN];
};

static struct frt_task_registry_entry g_frt_tasks[OVE_FRT_TASK_REGISTRY_MAX];
static UBaseType_t g_frt_task_count;

void ove_backend_freertos_task_created(void *task, const char *name)
{
	TaskHandle_t handle = (TaskHandle_t)task;

	if (handle == NULL)
		return;
	for (UBaseType_t i = 0; i < g_frt_task_count; ++i)
		if (g_frt_tasks[i].handle == handle)
			return;
	if (g_frt_task_count >= OVE_FRT_TASK_REGISTRY_MAX)
		return;

	struct frt_task_registry_entry *entry = &g_frt_tasks[g_frt_task_count++];
	entry->handle = handle;
	size_t i = 0;
	if (name != NULL) {
		while (i + 1u < sizeof(entry->name) && name[i] != '\0') {
			entry->name[i] = name[i];
			++i;
		}
	}
	entry->name[i] = '\0';
}

void ove_backend_freertos_task_deleted(void *task)
{
	TaskHandle_t handle = (TaskHandle_t)task;

	for (UBaseType_t i = 0; i < g_frt_task_count; ++i) {
		if (g_frt_tasks[i].handle != handle)
			continue;
		--g_frt_task_count;
		if (i != g_frt_task_count)
			g_frt_tasks[i] = g_frt_tasks[g_frt_task_count];
		g_frt_tasks[g_frt_task_count] = (struct frt_task_registry_entry){0};
		break;
	}
}
#endif

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
static uint32_t g_snapshot_window_calls;
static uint32_t g_snapshot_window_max_cycles;
static uint32_t g_snapshot_total_calls;
static uint32_t g_snapshot_total_max_cycles;

static void snapshot_metrics_record(uint32_t cycles)
{
	__atomic_add_fetch(&g_snapshot_window_calls, 1u, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_snapshot_total_calls, 1u, __ATOMIC_RELAXED);

	uint32_t observed = __atomic_load_n(&g_snapshot_window_max_cycles, __ATOMIC_RELAXED);
	while (cycles > observed &&
	       !__atomic_compare_exchange_n(&g_snapshot_window_max_cycles, &observed, cycles, 1,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
	observed = __atomic_load_n(&g_snapshot_total_max_cycles, __ATOMIC_RELAXED);
	while (cycles > observed &&
	       !__atomic_compare_exchange_n(&g_snapshot_total_max_cycles, &observed, cycles, 1,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

void ove_freertos_thread_snapshot_metrics_take(struct ove_freertos_thread_snapshot_metrics *window,
					       struct ove_freertos_thread_snapshot_metrics *total)
{
	window->calls = __atomic_exchange_n(&g_snapshot_window_calls, 0u, __ATOMIC_ACQ_REL);
	window->max_cycles =
		__atomic_exchange_n(&g_snapshot_window_max_cycles, 0u, __ATOMIC_ACQ_REL);
	total->calls = __atomic_load_n(&g_snapshot_total_calls, __ATOMIC_ACQUIRE);
	total->max_cycles = __atomic_load_n(&g_snapshot_total_max_cycles, __ATOMIC_ACQUIRE);
}
#endif

int ove_thread_list(struct ove_thread_info *out, size_t max_count, size_t *actual_count)
{
#if configUSE_TRACE_FACILITY
	UBaseType_t filled;
	configRUN_TIME_COUNTER_TYPE total_runtime;
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	uint32_t snapshot_start = DWT->CYCCNT;
#endif

	/*
	 * Task creation/deletion occurs only in task context, so suspending the
	 * scheduler makes registry handles safe without masking interrupts. Keep
	 * this interval bounded: copy scalar TCB data only and explicitly skip the
	 * linear stack-fill scan. A timer ISR can still release the scope task; it
	 * is dispatched as soon as xTaskResumeAll() runs.
	 */
	vTaskSuspendAll();
	filled = g_frt_task_count;
	if (out != NULL && filled > (UBaseType_t)max_count)
		filled = (UBaseType_t)max_count;
	total_runtime = portGET_RUN_TIME_COUNTER_VALUE();

	if (out != NULL) {
		for (UBaseType_t i = 0; i < filled; ++i) {
			TaskStatus_t task;
			TaskHandle_t handle = g_frt_tasks[i].handle;

			vTaskGetInfo(handle, &task, pdFALSE, eInvalid);
			out[i].name = g_frt_tasks[i].name;
			out[i].priority = (int)task.uxCurrentPriority;
			out[i].stack_size = 0u;
			out[i].stack_used = 0u;

			switch (task.eCurrentState) {
			case eRunning:
				out[i].state = OVE_THREAD_STATE_RUNNING;
				break;
			case eReady:
				out[i].state = OVE_THREAD_STATE_READY;
				break;
			case eBlocked:
				out[i].state = OVE_THREAD_STATE_BLOCKED;
				break;
			case eSuspended:
				out[i].state = OVE_THREAD_STATE_SUSPENDED;
				break;
			case eDeleted:
				out[i].state = OVE_THREAD_STATE_TERMINATED;
				break;
			default:
				out[i].state = OVE_THREAD_STATE_UNKNOWN;
				break;
			}

			out[i].cpu_percent_x100 =
				runtime_percent(task.ulRunTimeCounter, total_runtime);

#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
			uint64_t run_us = (uint64_t)task.ulRunTimeCounter * 1000u;
			uint64_t total_us = (uint64_t)total_runtime * 1000u;
#else
			uint64_t cycles_per_us = SystemCoreClock / 1000000u;
			uint64_t run_us = (uint64_t)task.ulRunTimeCounter / cycles_per_us;
			uint64_t total_us = (uint64_t)total_runtime / cycles_per_us;
#endif
			out[i].state_times.running_us = run_us;
			out[i].state_times.ready_us = 0u;
			out[i].state_times.blocked_us = total_us > run_us ? total_us - run_us : 0u;
			out[i].state_times.suspended_us = 0u;
		}
	}
	(void)xTaskResumeAll();
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	snapshot_metrics_record(DWT->CYCCNT - snapshot_start);
#endif
	if (actual_count)
		*actual_count = (size_t)filled;

	return OVE_OK;
#else
	(void)out;
	(void)max_count;
	(void)actual_count;
	return OVE_ERR_NOT_SUPPORTED;
#endif /* configUSE_TRACE_FACILITY */
}
