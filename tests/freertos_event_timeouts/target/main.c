/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include "ove/eventgroup.h"
#include "framework/semihosting_exit.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum scenario { LARGE, EXPIRE, HINT, POLL, FOREVER, GROUP_ABORT, NOTIFY_ABORT };
enum stage { STARTING, GROUP_WAIT, EVENT_REUSE };
struct user_region {
	ove_event_storage_t event_storage;
	ove_eventgroup_storage_t group_storage;
	ove_event_t event;
	ove_eventgroup_t group;
	TaskHandle_t supervisor;
	uint64_t timeout;
	unsigned scenario, entry, middle, exit;
	int result, second, group_result;
	atomic_uint done, stage;
};
_Static_assert(sizeof(struct user_region) <= 512, "complete user state must fit its region");
static struct user_region user __attribute__((aligned(512)));
static StaticTask_t worker_tcb, supervisor_tcb;
static StackType_t worker_stack[512] __attribute__((aligned(2048)));
static StackType_t supervisor_stack[1024];
static TaskHandle_t supervisor_task;
static unsigned checks, failures, workers;

static unsigned control(void)
{
	uint32_t value;
	__asm volatile("mrs %0, control" : "=r"(value));
	return value & 1;
}
void event_timeout_target_reject(void)
{
	semihosting_exit(3);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	event_timeout_target_reject();
}
static void check(bool ok, const char *name)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
static void worker(void *data)
{
	struct user_region *r = data;
	r->entry = control();
#if OVE_NATIVE_ABORT
	if (r->scenario == GROUP_ABORT) {
		atomic_store_explicit(&r->stage, GROUP_WAIT, memory_order_release);
		xTaskNotifyGive(r->supervisor);
		ove_eventbits_t bits = 0;
		r->group_result = ove_eventgroup_wait_bits(r->group, 1, 0, OVE_WAIT_FOREVER, &bits);
	}
#endif
	r->result = ove_event_wait(r->event, r->timeout);
	r->middle = control();
#if OVE_NATIVE_ABORT
	if (r->scenario == NOTIFY_ABORT) {
		atomic_store_explicit(&r->stage, EVENT_REUSE, memory_order_release);
		r->second = ove_event_wait(r->event, OVE_MS(50));
	}
#endif
	r->exit = control();
	atomic_store_explicit(&r->done, 1, memory_order_release);
	xTaskNotifyGive(r->supervisor);
	vTaskSuspend(NULL);
}
static TaskHandle_t start_worker(bool restricted)
{
	TaskHandle_t task = NULL;
#ifdef OVE_MPU_V1
	if (restricted) {
		TaskParameters_t parameters = {
			.pvTaskCode = worker,
			.pcName = "restricted",
			.usStackDepth = 512,
			.pvParameters = &user,
			.uxPriority = 4,
			.puxStackBuffer = worker_stack,
			.xRegions = {{.pvBaseAddress = &user,
				      .ulLengthInBytes = 512,
				      .ulParameters = portMPU_REGION_READ_WRITE |
						      portMPU_REGION_EXECUTE_NEVER}},
			.pxTaskBuffer = &worker_tcb,
		};
		if (xTaskCreateRestrictedStatic(&parameters, &task) != pdPASS)
			return NULL;
	} else {
		task = xTaskCreateStatic(worker, "privileged", 512, &user, 4 | portPRIVILEGE_BIT,
					 worker_stack, &worker_tcb);
	}
#else
	(void)restricted;
	task = xTaskCreateStatic(worker, "ordinary", 512, &user, 4, worker_stack, &worker_tcb);
#endif
	return task;
}
static void run(enum scenario scenario, bool restricted)
{
	memset(&user, 0, sizeof(user));
	atomic_init(&user.done, 0);
	atomic_init(&user.stage, STARTING);
	user.scenario = scenario;
	user.supervisor = supervisor_task;
	user.timeout = scenario == LARGE     ? UINT64_MAX - 1
		       : scenario == POLL    ? 0
		       : scenario == FOREVER ? OVE_WAIT_FOREVER
		       : scenario == EXPIRE  ? OVE_MS(5)
					     : OVE_MS(50);
	user.result = user.second = user.group_result = OVE_ERR_BUSY;
	int initialized = ove_event_init(&user.event, &user.event_storage);
	check(initialized == OVE_OK, "accessible native Event initializes");
	if (initialized != OVE_OK)
		event_timeout_target_reject();
	if (scenario == POLL)
		ove_event_signal(user.event);
#if OVE_NATIVE_ABORT
	if (scenario == GROUP_ABORT &&
	    ove_eventgroup_init(&user.group, &user.group_storage) != OVE_OK)
		event_timeout_target_reject();
#endif
	TaskHandle_t task = start_worker(restricted);
	check(task != NULL, "initialized native worker starts");
	if (!task)
		event_timeout_target_reject();
	++workers;
#if OVE_NATIVE_ABORT
	if (scenario == GROUP_ABORT) {
		if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) ||
		    atomic_load_explicit(&user.stage, memory_order_acquire) != GROUP_WAIT)
			event_timeout_target_reject();
		if (eTaskGetState(task) != eBlocked)
			event_timeout_target_reject();
		check(xTaskAbortDelay(task) == pdPASS,
		      "valid blocked EventGroup worker is aborted");
	} else if (scenario == NOTIFY_ABORT) {
		if (eTaskGetState(task) != eBlocked)
			event_timeout_target_reject();
		check(xTaskAbortDelay(task) == pdPASS, "valid notification worker is aborted");
		check(atomic_load_explicit(&user.stage, memory_order_acquire) == EVENT_REUSE,
		      "notification abort proceeds directly to Event reuse");
	}
#endif
	if (scenario != POLL && scenario != EXPIRE) {
		check(eTaskGetState(task) == eBlocked,
		      "finite or explicit forever Event really blocks");
		if (scenario == HINT) {
			xTaskNotifyGive(task);
			check(eTaskGetState(task) == eBlocked,
			      "unrelated hint reblocks without target latch");
		}
		/* Signal even when an old implementation returned early; all storage
		 * and the suspended worker remain live until completion is collected. */
		ove_event_signal(user.event);
	}
	if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) ||
	    atomic_load_explicit(&user.done, memory_order_acquire) != 1 ||
	    eTaskGetState(task) != eSuspended)
		event_timeout_target_reject();
	vTaskDelete(task);
#if OVE_NATIVE_ABORT
	if (scenario == GROUP_ABORT)
		ove_eventgroup_deinit(user.group);
#endif
	ove_event_deinit(user.event);
	check(user.entry == (unsigned)restricted && user.middle == (unsigned)restricted &&
		      user.exit == (unsigned)restricted,
	      "Event utilities preserve the native caller mode");
	check(user.result ==
		      (scenario == EXPIRE || scenario == NOTIFY_ABORT ? OVE_ERR_TIMEOUT : OVE_OK),
	      "native Event result matches its latch or timeout");
#if OVE_NATIVE_ABORT
	if (scenario == GROUP_ABORT)
		check(user.group_result == OVE_ERR_TIMEOUT,
		      "aborted EventGroup returned before the next Event operation");
	if (scenario == NOTIFY_ABORT)
		check(user.second == OVE_OK,
		      "Event remains usable after native notification abort");
#endif
	check(!control(), "supervisor remains privileged after worker cleanup");
}
static void supervisor(void *unused)
{
	(void)unused;
#if OVE_EVENT_TARGET_CANARY
	check(false, "early failure survives later native controls");
#endif
	check(!control(), "supervisor enters privileged");
#ifdef OVE_MPU_V1
	check((*(volatile uint32_t *)0xE000ED94 & 1) != 0, "real native MPU is enabled");
#endif
#ifndef CONFIG_OVE_ZERO_HEAP
	ove_event_t owned = NULL;
	int rc = ove_event_create(&owned);
	check(rc == OVE_OK && owned != NULL, "privileged common heap Event owner initializes");
	if (rc != OVE_OK || !owned)
		event_timeout_target_reject();
	ove_event_signal(owned);
	int result = ove_event_wait(owned, 0);
	ove_event_destroy(owned);
	check(result == OVE_OK, "privileged heap Event consumes its latch before cleanup");
#endif
	for (unsigned restricted = 0; restricted <= 1; ++restricted) {
#ifndef OVE_MPU_V1
		if (restricted)
			break;
#endif
		for (enum scenario scenario = LARGE; scenario <= FOREVER; ++scenario)
			run(scenario, restricted != 0);
#if OVE_NATIVE_ABORT
		run(GROUP_ABORT, restricted != 0);
		run(NOTIFY_ABORT, restricted != 0);
#endif
	}
	printf("freertos event native: mpu=%u abort=%u heap=%u ticks=%u workers=%u "
	       "checks=%u failures=%u\n",
#ifdef OVE_MPU_V1
	       1u,
#else
	       0u,
#endif
	       OVE_NATIVE_ABORT,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       OVE_NATIVE_TICK_BITS, workers, checks, failures);
	semihosting_exit(failures ? 1 : 0);
}
int main(void)
{
#ifdef OVE_MPU_V1
	if (((*(volatile uint32_t *)0xE000ED90 >> 8) & 0xff) != OVE_MPU_REGIONS)
		event_timeout_target_reject();
	const UBaseType_t priority = 2 | portPRIVILEGE_BIT;
#else
	const UBaseType_t priority = 2;
#endif
	supervisor_task = xTaskCreateStatic(supervisor, "supervisor", 1024, NULL, priority,
					    supervisor_stack, &supervisor_tcb);
	if (!supervisor_task)
		event_timeout_target_reject();
	vTaskStartScheduler();
	event_timeout_target_reject();
}
