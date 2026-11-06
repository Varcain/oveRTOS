/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <ove/sync.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef CONFIG_OVE_RTOS_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "framework/semihosting_exit.h"
typedef SemaphoreHandle_t gate_t;
static StaticSemaphore_t start_storage[2], ready_storage, done_storage;
static StaticTask_t task_storage[3];
static StackType_t task_stacks[3][8192];
static TaskHandle_t tasks[2];
#else
#include <zephyr/kernel.h>
#ifdef CONFIG_ARM
#include "framework/semihosting_exit.h"
#endif
typedef struct k_sem *gate_t;
static struct k_sem start_storage[2], ready_storage, done_storage;
K_THREAD_STACK_ARRAY_DEFINE(task_stacks, 2, 32768);
static struct k_thread task_storage[2];
#endif
static gate_t starts[2], ready, done;
static ove_mutex_storage_t mutex_storage;
static ove_condvar_storage_t cv_storage;
static ove_mutex_t mutex;
static ove_condvar_t cv;
static unsigned failures, scenarios, returns;
static struct {
	const char *name;
	uint64_t timeout;
	int expected, result;
	bool unrelated;
} jobs[2];

void condvar_target_reject(void)
{
#if defined(CONFIG_OVE_RTOS_FREERTOS) || defined(CONFIG_ARM)
	semihosting_exit(2);
#else
	exit(2);
#endif
}
static void check(bool condition, const char *name)
{
	if (!condition) {
		++failures;
		printf("failed: %s\n", name);
	}
}
static void take(gate_t gate)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	if (xSemaphoreTake(gate, pdMS_TO_TICKS(5000)) != pdTRUE)
#else
	if (k_sem_take(gate, K_SECONDS(5)))
#endif
		condvar_target_reject();
}
static void give(gate_t gate)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	if (xSemaphoreGive(gate) != pdTRUE)
		condvar_target_reject();
#else
	k_sem_give(gate);
#endif
}
static bool completed_now(void)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	return xSemaphoreTake(done, 0) == pdTRUE;
#else
	return k_sem_take(done, K_NO_WAIT) == 0;
#endif
}
static bool held_once(void)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	return xSemaphoreGetMutexHolder(mutex->sem) == xTaskGetCurrentTaskHandle();
#else
	return mutex->mtx.owner == k_current_get() && mutex->mtx.lock_count == 1;
#endif
}
#ifdef CONFIG_OVE_RTOS_ZEPHYR
static void check_stack(void)
{
	struct k_thread *thread = k_current_get();
	char live;
	const uintptr_t base = thread->stack_info.start;
	const size_t size = thread->stack_info.size;
	const uintptr_t address = (uintptr_t)&live;
	/* Capacity is a checked premise; linked-call budgeting remains separate. */
	if (size < 32768 || address < base || address - base >= size) {
		printf("rejected: actual thread stack bounds\n");
		condvar_target_reject();
	}
}
#endif
static void worker(unsigned index)
{
#ifdef CONFIG_OVE_RTOS_ZEPHYR
	check_stack();
#endif
	for (;;) {
		take(starts[index]);
		if (ove_mutex_lock(mutex, OVE_WAIT_FOREVER))
			condvar_target_reject();
#ifdef CONFIG_OVE_RTOS_FREERTOS
		if (jobs[index].unrelated)
			xTaskNotifyGive(xTaskGetCurrentTaskHandle());
#endif
		give(ready);
		jobs[index].result = ove_condvar_wait(cv, mutex, jobs[index].timeout);
		/* A bad ownership result rejects before any potentially unheld unlock. */
		if (!held_once()) {
			printf("rejected: %s worker=%u cause=ownership rc=%d\n", jobs[index].name,
			       index, jobs[index].result);
			condvar_target_reject();
		}
		if (jobs[index].result != jobs[index].expected) {
			printf("rejected: %s worker=%u cause=status rc=%d expected=%d\n",
			       jobs[index].name, index, jobs[index].result, jobs[index].expected);
			condvar_target_reject();
		}
		ove_mutex_unlock(mutex);
		give(done);
	}
}
#ifdef CONFIG_OVE_RTOS_FREERTOS
static void worker_entry(void *argument)
{
	worker((unsigned)(uintptr_t)argument);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	condvar_target_reject();
}
#else
static void worker_entry(void *argument, void *unused1, void *unused2)
{
	(void)unused1;
	(void)unused2;
	worker((unsigned)(uintptr_t)argument);
}
#endif
static void exercise(const char *name, unsigned count, uint64_t timeout, int expected, bool notify,
		     bool broadcast, bool held, bool unrelated)
{
	printf("scenario: %s workers=%u\n", name, count);
	for (unsigned i = 0; i < count; ++i) {
		jobs[i].name = name;
		jobs[i].timeout = timeout;
		jobs[i].expected = expected;
		jobs[i].result = 1;
		jobs[i].unrelated = unrelated;
		give(starts[i]);
	}
	for (unsigned i = 0; i < count; ++i)
		take(ready);
	if (notify) {
		/* Acquiring after ready proves each high-priority waiter released it. */
		if (ove_mutex_lock(mutex, OVE_WAIT_FOREVER))
			condvar_target_reject();
		if (!held)
			ove_mutex_unlock(mutex);
		if (broadcast)
			ove_condvar_broadcast(cv);
		else
			ove_condvar_signal(cv);
		if (held) {
			/* No waiter may finish while the notifier holds the caller mutex. */
			if (completed_now()) {
				printf("rejected: %s cause=completed-before-mutex-release\n", name);
				condvar_target_reject();
			}
			ove_mutex_unlock(mutex);
		}
	}
	for (unsigned i = 0; i < count; ++i)
		take(done);
	for (unsigned i = 0; i < count; ++i) {
		printf("returned: %s worker=%u rc=%d held=1\n", name, i, jobs[i].result);
		if (jobs[i].result != jobs[i].expected) {
			printf("rejected: %s worker=%u cause=status expected=%d\n", name, i,
			       jobs[i].expected);
			condvar_target_reject();
		}
		++returns;
	}
	++scenarios;
	printf("observed: %s workers=%u\n", name, count);
}
static void run(void)
{
#ifdef CONFIG_OVE_RTOS_ZEPHYR
	check_stack();
#endif
	if (ove_mutex_init(&mutex, &mutex_storage) || ove_condvar_init(&cv, &cv_storage))
		condvar_target_reject();
	if (mutex != &mutex_storage || cv != &cv_storage)
		condvar_target_reject();
#ifdef CONFIG_OVE_RTOS_FREERTOS
	if (!mutex->sem)
		condvar_target_reject();
	for (unsigned i = 0; i < 2; ++i)
		starts[i] = xSemaphoreCreateCountingStatic(1, 0, &start_storage[i]);
	ready = xSemaphoreCreateCountingStatic(2, 0, &ready_storage);
	done = xSemaphoreCreateCountingStatic(2, 0, &done_storage);
#else
	for (unsigned i = 0; i < 2; ++i) {
		if (k_sem_init(&start_storage[i], 0, 1))
			condvar_target_reject();
		starts[i] = &start_storage[i];
	}
	if (k_sem_init(&ready_storage, 0, 2) || k_sem_init(&done_storage, 0, 2))
		condvar_target_reject();
	ready = &ready_storage;
	done = &done_storage;
#endif
	if (!starts[0] || !starts[1] || !ready || !done)
		condvar_target_reject();
	for (unsigned i = 0; i < 2; ++i) {
#ifdef CONFIG_OVE_RTOS_FREERTOS
		tasks[i] = xTaskCreateStatic(worker_entry, "cv-worker", 8192, (void *)(uintptr_t)i,
					     3, task_stacks[i], &task_storage[i]);
		if (!tasks[i])
			condvar_target_reject();
#else
		if (!k_thread_create(&task_storage[i], task_stacks[i],
				     K_THREAD_STACK_SIZEOF(task_stacks[i]), worker_entry,
				     (void *)(uintptr_t)i, NULL, NULL, 5, 0, K_NO_WAIT))
			condvar_target_reject();
#endif
	}
#ifdef OVE_CONDVAR_TARGET_CANARY
	check(false, "native failure calibration");
#else
	check(true, "native failure calibration");
#endif
	ove_condvar_signal(cv);
	ove_condvar_broadcast(cv);
	exercise("zero-timeout", 1, 0, OVE_ERR_TIMEOUT, false, false, false, false);
	exercise("finite-timeout", 1, 2000000, OVE_ERR_TIMEOUT, false, false, false, false);
	exercise("signal", 1, OVE_WAIT_FOREVER, OVE_OK, true, false, false, false);
	exercise("broadcast", 2, OVE_WAIT_FOREVER, OVE_OK, true, true, false, false);
	exercise("held-signal", 1, OVE_WAIT_FOREVER, OVE_OK, true, false, true, false);
	exercise("held-broadcast", 2, OVE_WAIT_FOREVER, OVE_OK, true, true, true, false);
#ifdef CONFIG_OVE_RTOS_FREERTOS
	exercise("unrelated", 1, OVE_WAIT_FOREVER, OVE_OK, false, false, false, true);
#endif
	exercise("reuse", 2, OVE_WAIT_FOREVER, OVE_OK, true, true, false, false);
	ove_condvar_signal(cv);
	ove_condvar_broadcast(cv);
	ove_condvar_deinit(cv);
	ove_mutex_deinit(mutex);
	printf("native condvar: scenarios=%u returns=%u failures=%u\n", scenarios, returns,
	       failures);
#if defined(CONFIG_OVE_RTOS_FREERTOS) || defined(CONFIG_ARM)
	semihosting_exit(failures != 0);
#else
	exit(failures != 0);
#endif
}
#ifdef CONFIG_OVE_RTOS_FREERTOS
static void controller(void *argument)
{
	(void)argument;
	run();
}
int main(void)
{
	if (!xTaskCreateStatic(controller, "cv-controller", 8192, NULL, 2, task_stacks[2],
			       &task_storage[2]))
		condvar_target_reject();
	vTaskStartScheduler();
	return 2;
}
#else
int main(void)
{
	run();
	return 2;
}
#endif
