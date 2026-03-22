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

extern void xPortSysTickHandler(void);

/* Stub — tests exercise ove_app module without a real app entry point */
void ove_main(void) {}

static void test_runner_task(void *arg)
{
	int failures = 0;
	(void)arg;

	/*
	 * Run all functional tests under QEMU ARM emulation (zero-heap mode).
	 * FreeRTOS ARM_CM7/r0p1 port + stub backends for hardware modules.
	 * Static allocation only — no pvPortMalloc/vPortFree.
	 * Semihosting provides printf/exit.
	 */
	printf("=== Thread Tests ===\n");
	failures += test_thread_run();

	printf("=== Sync: Mutex Tests ===\n");
	failures += test_sync_mutex_run();

	printf("=== Sync: Semaphore Tests ===\n");
	failures += test_sync_sem_run();

	printf("=== Sync: Event Tests ===\n");
	failures += test_sync_event_run();

	printf("=== Sync: Condvar Tests ===\n");
	failures += test_sync_condvar_run();

	printf("=== Sync: Recursive Mutex Tests ===\n");
	failures += test_sync_recursive_run();

	printf("=== Queue Tests ===\n");
	failures += test_queue_run();

	printf("=== Timer Tests ===\n");
	failures += test_timer_run();

	printf("=== Time Tests ===\n");
	failures += test_time_run();

	printf("=== EventGroup Tests ===\n");
	failures += test_eventgroup_run();

	printf("=== Workqueue Tests ===\n");
	failures += test_workqueue_run();

	printf("=== Stream Tests ===\n");
	failures += test_stream_run();

	printf("=== Console Tests ===\n");
	failures += test_console_run();

	printf("=== Watchdog Tests ===\n");
	failures += test_watchdog_run();

	printf("=== NVS Tests ===\n");
	failures += test_nvs_run();

	printf("=== Shell Tests ===\n");
	failures += test_shell_run();

	printf("=== Audio Tests ===\n");
	failures += test_audio_run();

	printf("=== BSP Tests ===\n");
	failures += test_bsp_run();

	printf("=== Board Tests ===\n");
	failures += test_board_run();

	printf("=== GPIO Tests ===\n");
	failures += test_gpio_run();

	printf("=== LED Tests ===\n");
	failures += test_led_run();

	/* FS tests skipped — no filesystem on bare-metal QEMU */

	printf("=== LVGL Tests ===\n");
	failures += test_lvgl_run();

	printf("=== App Tests ===\n");
	failures += test_app_run();

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);

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

/* Static storage for the test runner task */
static StaticTask_t runner_tcb;
static StackType_t runner_stack[configMINIMAL_STACK_SIZE * 8];

int main(void)
{
	xTaskCreateStatic(test_runner_task, "tests",
			  configMINIMAL_STACK_SIZE * 8, NULL,
			  tskIDLE_PRIORITY + 1,
			  runner_stack, &runner_tcb);
	vTaskStartScheduler();
	return 0;
}
