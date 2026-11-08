/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/eventgroup.h"
#include "framework/semihosting_exit.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

struct user_region {
	ove_eventgroup_storage_t storage;
	TaskHandle_t supervisor;
	uint32_t entry, exit;
	atomic_uint done;
};
_Static_assert(sizeof(struct user_region) <= 256, "complete user state must fit its MPU region");
static struct user_region user __attribute__((aligned(256)));
static StaticTask_t user_tcb, supervisor_tcb;
static StackType_t user_stack[512] __attribute__((aligned(2048)));
static StackType_t supervisor_stack[1024];
static TaskHandle_t supervisor_task;
static unsigned checks, failures;

static uint32_t control(void)
{
	uint32_t value;
	__asm volatile("mrs %0, control" : "=r"(value));
	return value;
}
void eventgroup_target_reject(void)
{
	semihosting_exit(3);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	eventgroup_target_reject();
}
static void check(bool ok, const char *name)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
static void restricted(void *data)
{
	struct user_region *r = data;
	r->entry = control() & 1;
	ove_eventgroup_deinit((ove_eventgroup_t)&r->storage);
	r->exit = control() & 1;
	atomic_store_explicit(&r->done, 1, memory_order_release);
	xTaskNotifyGive(r->supervisor);
	vTaskSuspend(NULL);
}
static void supervisor(void *unused)
{
	(void)unused;
#if OVE_DEFERRED_CANARY
	check(false, "early failure survives later controls");
#endif
	check(!(control() & 1), "supervisor is privileged");
	check((*(volatile uint32_t *)0xE000ED94 & 1) != 0, "native MPU is enabled");
	ove_eventgroup_t group = NULL;
	int rc = ove_eventgroup_init(&group, &user.storage);
	check(rc == OVE_OK && group == &user.storage, "complete accessible group initializes");
	if (rc != OVE_OK)
		semihosting_exit(1);
	user.supervisor = supervisor_task;
	TaskParameters_t parameters = {
		.pvTaskCode = restricted,
		.pcName = "restricted",
		.usStackDepth = 512,
		.pvParameters = &user,
		.uxPriority = 4,
		.puxStackBuffer = user_stack,
		.xRegions = {{.pvBaseAddress = &user,
			      .ulLengthInBytes = 256,
			      .ulParameters = portMPU_REGION_READ_WRITE |
					      portMPU_REGION_EXECUTE_NEVER}},
		.pxTaskBuffer = &user_tcb,
	};
	TaskHandle_t worker = NULL;
	rc = xTaskCreateRestrictedStatic(&parameters, &worker);
	check(rc == pdPASS && worker != NULL, "native non-program restricted task starts");
	if (rc != pdPASS || !worker)
		semihosting_exit(1);
	check(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) != 0 &&
		      atomic_load_explicit(&user.done, memory_order_acquire) == 1,
	      "ordinary task teardown completes through the native entry");
	check(user.entry == 1 && user.exit == 1, "deinit preserves the unprivileged task mode");
	vTaskDelete(worker);
	check(ove_eventgroup_init(&group, &user.storage) == OVE_OK,
	      "quiescent caller storage reopens after native task completion");
	ove_eventgroup_deinit(group);
	check(!(control() & 1), "ordinary privileged teardown preserves supervisor mode");
	printf("eventgroup deferred mpu: checks=%u failures=%u entry=%u exit=%u regions=%u\n",
	       checks, failures, (unsigned)user.entry, (unsigned)user.exit,
	       (unsigned)((*(volatile uint32_t *)0xE000ED90 >> 8) & 0xff));
	semihosting_exit(failures ? 1 : 0);
}
int main(void)
{
	if (((*(volatile uint32_t *)0xE000ED90 >> 8) & 0xff) != OVE_MPU_REGIONS)
		semihosting_exit(2);
	supervisor_task = xTaskCreateStatic(supervisor, "supervisor", 1024, NULL,
					    2 | portPRIVILEGE_BIT, supervisor_stack,
					    &supervisor_tcb);
	if (!supervisor_task)
		semihosting_exit(1);
	vTaskStartScheduler();
	semihosting_exit(2);
}
