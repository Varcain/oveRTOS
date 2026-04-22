/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared FreeRTOS hooks for oveRTOS.
 *
 * Added to every FreeRTOS build via ove_add_freertos_kernel().  Provides
 * the static-allocation memory providers and a default (spin) stack
 * overflow hook.  All functions are weak so boards can override when they
 * have working I/O and want to print diagnostics.
 *
 * Exception handlers (HardFault_Handler, NMI_Handler, etc.) are NOT
 * defined here — they are provided by the Cortex-M startup file as weak
 * aliases to Default_Handler.  Boards with working I/O may override
 * HardFault_Handler from their main.c for better diagnostics.
 *
 * SysTick_Handler is always board-local because the tick routing varies
 * (STM32 boards need HAL_IncTick() before xPortSysTickHandler(); QEMU
 * boards call xPortSysTickHandler() directly).
 */

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#if defined(__GNUC__)
#define OVE_WEAK __attribute__((weak))
#else
#define OVE_WEAK
#endif

/* ========================================================================= */
/* STATIC ALLOCATION HOOKS                                                   */
/* ========================================================================= */

#if (configSUPPORT_STATIC_ALLOCATION == 1)

OVE_WEAK
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

#if (configUSE_TIMERS == 1)
OVE_WEAK
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
#endif /* configUSE_TIMERS */

#endif /* configSUPPORT_STATIC_ALLOCATION */

/* ========================================================================= */
/* STACK OVERFLOW HOOK                                                       */
/* ========================================================================= */

#if (configCHECK_FOR_STACK_OVERFLOW > 0)

OVE_WEAK
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	(void)pcTaskName;
	/*
	 * Default: spin.  Boards with working I/O (serial UART, semihosting)
	 * should override this in main.c to print the task name before
	 * halting, which is much more useful for debugging.
	 */
	for (;;) {
		__asm volatile("nop");
	}
}

#endif /* configCHECK_FOR_STACK_OVERFLOW */

/* ========================================================================= */
/* RUN-TIME STATS COUNTER (QEMU — DWT not emulated)                         */
/* ========================================================================= */

#if (configGENERATE_RUN_TIME_STATS == 1)
/* Millisecond counter incremented from vApplicationTickHook().
 * Used by portGET_RUN_TIME_COUNTER_VALUE() on boards where the
 * DWT cycle counter is not available (e.g. QEMU). */
volatile uint32_t ove_runtime_counter_ms;
#endif

/* ========================================================================= */
/* TICK HOOK                                                                 */
/* ========================================================================= */

#if (configUSE_TICK_HOOK == 1)

#ifdef CONFIG_OVE_PROFILER
/* Provided by backends/freertos/freertos_profiler.c. Runs in SysTick ISR
 * context and samples the interrupted task's stacked PC. Weak so builds
 * without the profiler backend (plugged via Kconfig gating of that file)
 * still link. */
__attribute__((weak)) void ove_backend_profiler_on_tick(void) { }
#endif

OVE_WEAK
void vApplicationTickHook(void)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
	ove_runtime_counter_ms++;
#endif
#ifdef CONFIG_OVE_PROFILER
	ove_backend_profiler_on_tick();
#endif
}
#endif
