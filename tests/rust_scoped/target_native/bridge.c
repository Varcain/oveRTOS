/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/ove.h"
#include "ove/storage.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#if !defined(CONFIG_OVE_SYNC) || !defined(CONFIG_OVE_ZERO_HEAP) || defined(CONFIG_OVE_ASYNC) || \
	defined(CONFIG_OVE_TIME) || defined(CONFIG_OVE_CONSOLE) || defined(CONFIG_SMP)
#error "native scoped control requires single-core zero-heap SYNC only"
#endif
#if !defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500) || defined(CONFIG_OVE_LINUX) ||              \
	defined(CONFIG_OVE_LVGL) || defined(CONFIG_OVE_AUDIO) || defined(CONFIG_OVE_NET) || \
	defined(CONFIG_OVE_ASYNC_NET) || defined(CONFIG_OVE_TRACE_STREAM) ||                \
	defined(CONFIG_OVE_PROFILER)
#error "native scoped control requires the isolated AN500 kernel profile"
#endif
#ifdef CONFIG_OVE_RTOS_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#if configUSE_PREEMPTION != 1 || configSUPPORT_STATIC_ALLOCATION != 1 ||                 \
	INCLUDE_vTaskDelete != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1 ||             \
	INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xTaskGetIdleTaskHandle != 1 || \
	INCLUDE_xTaskGetSchedulerState != 1 || configMAX_PRIORITIES < 4
#error "native scoped control requires current-task static mutex and deletion support"
#endif
#if defined(configNUMBER_OF_CORES) && configNUMBER_OF_CORES != 1
#error "native scoped control requires one native processor"
#endif
typedef SemaphoreHandle_t gate_t;
static StaticSemaphore_t start_storage[2], done_storage;
static StaticTask_t task_storage[3];
static StackType_t stacks[3][32768];
static TaskHandle_t tasks[3];
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/kernel.h>
#if !defined(CONFIG_MULTITHREADING) || !defined(CONFIG_THREAD_STACK_INFO) || \
	defined(CONFIG_SMP) || defined(CONFIG_OVE_SHMEM_DISPLAY)
#error "native scoped control requires thread stack descriptors"
#endif
typedef struct k_sem *gate_t;
static struct k_sem start_storage[2], done_storage;
static struct k_thread task_storage[3];
K_THREAD_STACK_ARRAY_DEFINE(stacks, 3, 131072);
#else
#error "native scoped control supports FreeRTOS and Zephyr"
#endif
static gate_t starts[2], done;
static ove_mutex_t observed[2];
static bool capturing, recursive_initializing;
static unsigned captures[2], rounds, completions;
static ove_mutex_t expected_backing[2];
static _Atomic unsigned failures, returns, retirements, steps;
static _Atomic unsigned probes[2];
static _Atomic unsigned isolated_starts;
extern int scoped_native_initialize(void);
extern int scoped_native_step(uint32_t);
extern void scoped_native_shutdown(void);
int __real_ove_mutex_init(ove_mutex_t *, ove_mutex_storage_t *);
int __real_ove_recursive_mutex_init(ove_mutex_t *, ove_mutex_storage_t *);

/* Keep this finite kernel control independent of the simulation transport,
 * global host shared-memory files and its additional debug worker. */
int __wrap_ove_sim_board_init(void)
{
	atomic_fetch_add(&isolated_starts, 1);
	return OVE_OK;
}

_Noreturn void scoped_native_reject(void)
{
	ove_app_exit(2);
	for (;;) {
	}
}
static void check(bool valid, const char *message)
{
	if (!valid) {
		atomic_fetch_add(&failures, 1);
		printf("failed: %s\n", message);
	}
}
int __wrap_ove_mutex_init(ove_mutex_t *out, ove_mutex_storage_t *storage)
{
	int rc = __real_ove_mutex_init(out, storage);
	if (capturing && !recursive_initializing && rc == OVE_OK) {
		++captures[0];
		observed[0] = *out;
	}
	return rc;
}
int __wrap_ove_recursive_mutex_init(ove_mutex_t *out, ove_mutex_storage_t *storage)
{
	recursive_initializing = true;
	int rc = __real_ove_recursive_mutex_init(out, storage);
	recursive_initializing = false;
	if (capturing && rc == OVE_OK) {
		++captures[1];
		observed[1] = *out;
	}
	return rc;
}
static void take(gate_t gate)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	if (xSemaphoreTake(gate, pdMS_TO_TICKS(5000)) != pdTRUE)
#else
	if (k_sem_take(gate, K_SECONDS(5)))
#endif
		scoped_native_reject();
}
static void give(gate_t gate)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	if (xSemaphoreGive(gate) != pdTRUE)
		scoped_native_reject();
#else
	k_sem_give(gate);
#endif
}
static bool unheld(ove_mutex_t mutex)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	return xSemaphoreGetMutexHolder(mutex->sem) == NULL;
#else
	return mutex->mtx.owner == NULL && mutex->mtx.lock_count == 0;
#endif
}
static bool owned(ove_mutex_t mutex)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	return xSemaphoreGetMutexHolder(mutex->sem) == xTaskGetCurrentTaskHandle();
#else
	return mutex->mtx.owner == k_current_get() && mutex->mtx.lock_count != 0;
#endif
}
void scoped_native_probe(uint32_t kind)
{
	if (kind >= 2 || observed[kind] == NULL || !owned(observed[kind]))
		scoped_native_reject();
	atomic_fetch_add(&probes[kind], 1);
}
static void retire_current(void)
{
	for (unsigned kind = 0; kind < 2; ++kind) {
		bool debt = !unheld(observed[kind]);
		check(!debt, "scope cleanup precedes native worker retirement");
		if (!debt)
			continue;
		if (!owned(observed[kind]))
			scoped_native_reject();
		for (unsigned level = 0; level < 2 && !unheld(observed[kind]); ++level) {
			if (kind)
				ove_recursive_mutex_unlock(observed[kind]);
			else
				ove_mutex_unlock(observed[kind]);
		}
		if (!unheld(observed[kind]))
			scoped_native_reject();
		atomic_fetch_add(&retirements, 1);
	}
}
static void check_stack(void)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	_Static_assert(sizeof(StackType_t) == 4, "native stack word width");
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	char live;
	bool matched = false;
	for (unsigned index = 0; index < 3; ++index) {
		uintptr_t base = (uintptr_t)stacks[index], address = (uintptr_t)&live;
		if (current == tasks[index])
			matched = address >= base && address - base < sizeof(stacks[index]);
	}
	if (!matched)
		scoped_native_reject();
#else
	struct k_thread *thread = k_current_get();
	char live;
	uintptr_t base = thread->stack_info.start, address = (uintptr_t)&live;
	if (thread->stack_info.size < 131072 || address < base ||
	    address - base >= thread->stack_info.size)
		scoped_native_reject();
#endif
}
static void step(unsigned index)
{
	check_stack();
	check((ove_sync_context() & (OVE_SYNC_CONTEXT_RESOURCE | OVE_SYNC_CONTEXT_BLOCKING)) ==
		      (OVE_SYNC_CONTEXT_RESOURCE | OVE_SYNC_CONTEXT_BLOCKING),
	      "native worker has supported resource and blocking context");
	check(scoped_native_step(index) == 0, "actual Rust scoped body completes");
	retire_current();
	atomic_fetch_add(&steps, 1);
}
static void worker(unsigned index)
{
	take(starts[index]);
	step(index);
	atomic_fetch_add(&returns, 1);
	give(done);
#ifdef CONFIG_OVE_RTOS_FREERTOS
	/* Higher priority than coordinator: deletion precedes its next dispatch. */
	vTaskDelete(NULL);
#endif
}
#ifdef CONFIG_OVE_RTOS_FREERTOS
static void worker_entry(void *argument)
{
	worker((unsigned)(uintptr_t)argument);
}
#else
static void worker_entry(void *argument, void *unused1, void *unused2)
{
	(void)unused1;
	(void)unused2;
	worker((unsigned)(uintptr_t)argument);
}
#endif
static void initialize(void)
{
	observed[0] = observed[1] = NULL;
	captures[0] = captures[1] = 0;
	capturing = true;
	int result = scoped_native_initialize();
	capturing = false;
	if (result || captures[0] != 1 || captures[1] != 1 || observed[0] == NULL ||
	    observed[1] == NULL || observed[0] == observed[1])
		scoped_native_reject();
	for (unsigned kind = 0; kind < 2; ++kind) {
		if (rounds == 0)
			expected_backing[kind] = observed[kind];
		else if (expected_backing[kind] != observed[kind])
			scoped_native_reject();
	}
	++rounds;
}
static void coordinator(void)
{
	check_stack();
	if (atomic_load(&isolated_starts) != 1)
		scoped_native_reject();
#ifdef OVE_SCOPED_NATIVE_CANARY
	check(false, "explicit early failure canary");
#endif
	initialize();
	for (unsigned index = 0; index < 2; ++index) {
		give(starts[index]);
		take(done);
#ifdef CONFIG_OVE_RTOS_FREERTOS
		/* Both workers have priority3; coordinator has priority2. A worker
		 * cannot yield back to it between give(done) and self deletion. */
#else
		if (k_thread_join(&task_storage[index], K_SECONDS(5)))
			scoped_native_reject();
#endif
		check(unheld(observed[0]) && unheld(observed[1]),
		      "completed worker leaves no native lock owner");
		++completions;
	}
	step(0);
	scoped_native_shutdown();
	initialize();
	step(1);
	scoped_native_shutdown();
	check(atomic_load(&probes[0]) == 10 && atomic_load(&probes[1]) == 12,
	      "every ordinary and recursive callback stage was observed");
	printf("scoped-native: failures=%u workers=%u completions=%u steps=%u retirements=%u init_rounds=%u probes=%u,%u isolated_starts=%u\n",
	       atomic_load(&failures), atomic_load(&returns), completions, atomic_load(&steps),
	       atomic_load(&retirements), rounds, atomic_load(&probes[0]), atomic_load(&probes[1]),
	       atomic_load(&isolated_starts));
	ove_app_exit(atomic_load(&failures) ? 1 : 0);
	scoped_native_reject();
}
#ifdef CONFIG_OVE_RTOS_FREERTOS
static void coordinator_entry(void *unused)
{
	(void)unused;
	coordinator();
}
#else
static void coordinator_entry(void *unused, void *unused1, void *unused2)
{
	(void)unused;
	(void)unused1;
	(void)unused2;
	coordinator();
}
#endif
void scoped_native_start(void)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	for (unsigned index = 0; index < 2; ++index)
		starts[index] = xSemaphoreCreateCountingStatic(1, 0, &start_storage[index]);
	done = xSemaphoreCreateCountingStatic(1, 0, &done_storage);
	if (!starts[0] || !starts[1] || !done)
		scoped_native_reject();
	for (unsigned index = 0; index < 2; ++index) {
		tasks[index] = xTaskCreateStatic(worker_entry, "scope-worker", 32768,
						 (void *)(uintptr_t)index, 3, stacks[index],
						 &task_storage[index]);
		if (!tasks[index])
			scoped_native_reject();
	}
	tasks[2] = xTaskCreateStatic(coordinator_entry, "scope-main", 32768, NULL, 2, stacks[2],
				     &task_storage[2]);
	if (!tasks[2])
		scoped_native_reject();
	vTaskStartScheduler();
	scoped_native_reject();
#else
	for (unsigned index = 0; index < 2; ++index) {
		if (k_sem_init(&start_storage[index], 0, 1))
			scoped_native_reject();
		starts[index] = &start_storage[index];
	}
	if (k_sem_init(&done_storage, 0, 1))
		scoped_native_reject();
	done = &done_storage;
	for (unsigned index = 0; index < 2; ++index)
		k_thread_create(&task_storage[index], stacks[index],
				K_THREAD_STACK_SIZEOF(stacks[index]), worker_entry,
				(void *)(uintptr_t)index, NULL, NULL, 3, 0, K_NO_WAIT);
	k_thread_create(&task_storage[2], stacks[2], K_THREAD_STACK_SIZEOF(stacks[2]),
			coordinator_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
#endif
}
