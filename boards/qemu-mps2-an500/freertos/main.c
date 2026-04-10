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
 *
 * Shared FreeRTOS hooks (static-allocation task memory, default stack
 * overflow hook) live in backends/freertos/freertos_hooks.c.  Default
 * ARM Cortex-M exception handlers are weak aliases to Default_Handler
 * provided by startup_CMSDK_CM7.s.  This file overrides HardFault_Handler
 * and vApplicationStackOverflowHook to print diagnostics via semihosting,
 * and provides the SysTick_Handler (QEMU has no HAL tick to increment).
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

/* Diagnostic overrides (strong symbols override the weak defaults in
 * freertos_hooks.c / startup_CMSDK_CM7.s). */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	fprintf(stderr, "\n!!! STACK OVERFLOW: %s !!!\n",
		pcTaskName ? pcTaskName : "(null)");
	while (1) {
		__asm volatile("nop");
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
