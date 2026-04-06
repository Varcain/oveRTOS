/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU MPS2-AN500 FreeRTOS entry point.
 * Semihosting provides printf/exit — no UART or HAL needed.
 */

#include "ove/ove.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

extern void xPortSysTickHandler(void);
extern void initialise_monitor_handles(void);
extern int ove_sim_board_init(void);

int main(void)
{
	initialise_monitor_handles();
	ove_sim_board_init();
	ove_app_run();
	return 0;
}

/* ========================================================================= */
/* FREERTOS HOOKS                                                            */
/* ========================================================================= */

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

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	fprintf(stderr, "\n!!! STACK OVERFLOW: %s !!!\n",
		pcTaskName ? pcTaskName : "(null)");
	while (1) {
		__asm volatile("nop");
	}
}

/* ========================================================================= */
/* EXCEPTION HANDLERS                                                        */
/* ========================================================================= */

void NMI_Handler(void)
{
	while (1) {
	}
}

void HardFault_Handler(void)
{
	fprintf(stderr, "\n!!! HARD FAULT !!!\n");
	fflush(stderr);
	while (1) {
		__asm volatile("nop");
	}
}

void MemManage_Handler(void)
{
	while (1) {
	}
}

void BusFault_Handler(void)
{
	while (1) {
	}
}

void UsageFault_Handler(void)
{
	while (1) {
	}
}

void DebugMon_Handler(void)
{
	while (1) {
	}
}

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
