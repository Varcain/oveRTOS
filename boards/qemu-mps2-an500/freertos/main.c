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
	volatile uint32_t cfsr  = *(volatile uint32_t *)0xE000ED28;
	volatile uint32_t hfsr  = *(volatile uint32_t *)0xE000ED2C;
	volatile uint32_t mmfar = *(volatile uint32_t *)0xE000ED34;
	volatile uint32_t bfar  = *(volatile uint32_t *)0xE000ED38;
	uint32_t msp, psp, lr_ret;
	__asm volatile("mrs %0, msp"   : "=r"(msp));
	__asm volatile("mrs %0, psp"   : "=r"(psp));
	__asm volatile("mov %0, lr"    : "=r"(lr_ret));
	fprintf(stderr,
		"\n!!! HARD FAULT !!!\n"
		"  CFSR=0x%08lx HFSR=0x%08lx MMFAR=0x%08lx BFAR=0x%08lx\n"
		"  MSP=0x%08lx PSP=0x%08lx LR=0x%08lx\n",
		(unsigned long)cfsr, (unsigned long)hfsr,
		(unsigned long)mmfar, (unsigned long)bfar,
		(unsigned long)msp, (unsigned long)psp,
		(unsigned long)lr_ret);
	/* Stacked frame at (MSP or PSP based on LR EXC_RETURN SPSEL bit). */
	uint32_t *sp = (lr_ret & 0x4) ? (uint32_t *)psp : (uint32_t *)msp;
	fprintf(stderr,
		"  stacked r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx\n"
		"  stacked r12=0x%08lx LR=0x%08lx PC=0x%08lx xPSR=0x%08lx\n",
		(unsigned long)sp[0], (unsigned long)sp[1],
		(unsigned long)sp[2], (unsigned long)sp[3],
		(unsigned long)sp[4], (unsigned long)sp[5],
		(unsigned long)sp[6], (unsigned long)sp[7]);
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
