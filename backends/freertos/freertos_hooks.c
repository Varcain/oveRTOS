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

#include "ove_config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "ove_freertos_tick.h"

/*
 * PM runs in its own polling thread.  Keep the larger idle stack from
 * when PM callbacks ran in the idle hook.
 */
#ifdef CONFIG_OVE_PM
#define OVE_IDLE_STACK_DEPTH ((configMINIMAL_STACK_SIZE) > 512 ? (configMINIMAL_STACK_SIZE) : 512)
#else
#define OVE_IDLE_STACK_DEPTH (configMINIMAL_STACK_SIZE)
#endif

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
	static StackType_t uxIdleTaskStack[OVE_IDLE_STACK_DEPTH];
	*ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
	*ppxIdleTaskStackBuffer = uxIdleTaskStack;
	*pulIdleTaskStackSize = OVE_IDLE_STACK_DEPTH;
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
/* IDLE HOOK                                                                 */
/* ========================================================================= */

#if configUSE_IDLE_HOOK
/* Applications may override this weak empty default. */
OVE_WEAK
void vApplicationIdleHook(void)
{
}
#endif

#if defined(CONFIG_OVE_SIM_WEB) && !defined(CONFIG_OVE_PM) && configUSE_TICKLESS_IDLE
/*
 * A spinning guest keeps QEMU's WebAssembly TCG interpreter busy on a
 * host core.  FreeRTOS calls this when it expects at least two idle ticks.
 * SysTick keeps running.  Mask interrupts across the check so an IRQ
 * that readies a task either aborts sleep or stays pending and wakes WFI.
 */
void ove_freertos_idle_wfi(void)
{
	__asm volatile("cpsid i" ::: "memory");
	if (eTaskConfirmSleepModeStatus() != eAbortSleep) {
		__asm volatile("dsb\n\twfi\n\tisb" ::: "memory");
	}
	__asm volatile("cpsie i" ::: "memory");
}
#endif

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
/* TICK HOOK                                                                 */
/* ========================================================================= */

#if (configUSE_TICK_HOOK == 1)

static ove_freertos_tick_callback_t volatile g_tick_callback;

int ove_freertos_tick_subscribe(ove_freertos_tick_callback_t callback)
{
	if (!callback)
		return -1;
	taskENTER_CRITICAL();
	int result = g_tick_callback && g_tick_callback != callback ? -1 : 0;
	if (result == 0)
		g_tick_callback = callback;
	taskEXIT_CRITICAL();
	return result;
}

void ove_freertos_tick_unsubscribe(ove_freertos_tick_callback_t callback)
{
	taskENTER_CRITICAL();
	if (g_tick_callback == callback)
		g_tick_callback = NULL;
	taskEXIT_CRITICAL();
}

/* Advances the 64-bit wrap-stitched nanosecond clock. The strong version lives in
 * freertos_time.c (real HW, with the DWT cycle counter); a weak no-op here keeps
 * builds on freertos_time_systick.c (QEMU, Renode) linking. */
__attribute__((weak)) void ove_freertos_time_tick(void)
{
}

OVE_WEAK
void vApplicationTickHook(void)
{
	/* Sampling CYCCNT every 1 ms catches its ~19.86 s wrap so CLOCK_MONOTONIC stays 64-bit. */
	ove_freertos_time_tick();
	ove_freertos_tick_callback_t callback = g_tick_callback;
	if (callback)
		callback();
}
#endif
