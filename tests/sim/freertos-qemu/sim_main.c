/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "framework/ove_test.h"
#include "framework/semihosting_exit.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef OVE_COVERAGE
extern void __gcov_dump(void);
#endif

extern void xPortSysTickHandler(void);

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void) {}

static void test_runner_task(void *arg)
{
	int failures = 0;
	(void)arg;

	/*
	 * Run all functional tests under QEMU ARM emulation.
	 * FreeRTOS ARM_CM7/r0p1 port + stub backends for hardware modules.
	 * Semihosting provides printf/exit.
	 * FS tests skipped — no filesystem on bare-metal QEMU.
	 */
#define OVE_SUITE(name, label) \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);

#ifdef OVE_COVERAGE
	__gcov_dump();
#endif
	semihosting_exit(failures ? 1 : 0);
}

/* Static allocation callbacks required by configSUPPORT_STATIC_ALLOCATION */
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
				   StackType_t **ppxIdleTaskStackBuffer,
				   configSTACK_DEPTH_TYPE *pulIdleTaskStackSize)
{
	static StaticTask_t xIdleTaskTCB;
	static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];
	*ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
	*ppxIdleTaskStackBuffer = uxIdleTaskStack;
	*pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
				    StackType_t **ppxTimerTaskStackBuffer,
				    configSTACK_DEPTH_TYPE *pulTimerTaskStackSize)
{
	static StaticTask_t xTimerTaskTCB;
	static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];
	*ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
	*ppxTimerTaskStackBuffer = uxTimerTaskStack;
	*pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

/* FreeRTOS hooks and exception handlers */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	fprintf(stderr, "\n!!! STACK OVERFLOW: %s !!!\n",
		pcTaskName ? pcTaskName : "(null)");
	semihosting_exit(1);
}

void NMI_Handler(void)        { while (1) {} }
void HardFault_Handler(void)  { while (1) {} }
void MemManage_Handler(void)  { while (1) {} }
void BusFault_Handler(void)   { while (1) {} }
void UsageFault_Handler(void) { while (1) {} }
void DebugMon_Handler(void)   { while (1) {} }

void SysTick_Handler(void)
{
#if (INCLUDE_xTaskGetSchedulerState == 1)
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
#endif
		xPortSysTickHandler();
#if (INCLUDE_xTaskGetSchedulerState == 1)
	}
#endif
}

int main(void)
{
	xTaskCreate(test_runner_task, "tests",
		    configMINIMAL_STACK_SIZE * 8, NULL,
		    tskIDLE_PRIORITY + 1, NULL);
	vTaskStartScheduler();
	return 0;
}
