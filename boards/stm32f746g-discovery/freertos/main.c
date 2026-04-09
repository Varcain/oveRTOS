/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "FreeRTOS.h"
#include "task.h"
#include "serial_wrapper.h"
#include "stm32f7xx_hal.h"

#if configSUPPORT_DYNAMIC_ALLOCATION
/* FreeRTOS heap — use dedicated .RamData2 section (64 KB) when the heap fits,
 * otherwise fall back to main RAM (.bss).
 * Not allocated at all in zero-heap mode (configSUPPORT_DYNAMIC_ALLOCATION=0). */
#if defined(HAL_ETH_MODULE_ENABLED) || configTOTAL_HEAP_SIZE > 0x10000
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((aligned(8)));
#else
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((section(".RamData2")));
#endif
#endif /* configSUPPORT_DYNAMIC_ALLOCATION */

extern void xPortSysTickHandler(void);

int main(void)
{
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

    serial_safe_write("\n!!! STACK OVERFLOW: ", 21);
    if (pcTaskName != NULL) {
        int len = 0;
        while (pcTaskName[len] != '\0' && len < 16) {
            len++;
        }
        serial_safe_write(pcTaskName, len);
    }
    serial_safe_write(" !!!\n", 5);

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
    while (1) {
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
    HAL_IncTick();

#if (INCLUDE_xTaskGetSchedulerState == 1)
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
#endif
        xPortSysTickHandler();
#if (INCLUDE_xTaskGetSchedulerState == 1)
    }
#endif
}
