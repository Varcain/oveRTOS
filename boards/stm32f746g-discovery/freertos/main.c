/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * STM32F746G-Discovery FreeRTOS entry point.
 *
 * Shared FreeRTOS hooks (static-allocation task memory, default stack
 * overflow hook) live in backends/freertos/freertos_hooks.c.  Default
 * ARM Cortex-M exception handlers are weak aliases to Default_Handler
 * provided by startup_stm32f746xx.s.  This file overrides
 * vApplicationStackOverflowHook to print the task name via UART before
 * halting, and provides SysTick_Handler which must call HAL_IncTick()
 * before xPortSysTickHandler().
 *
 * The ucHeap placement logic below steers the FreeRTOS heap to the most
 * appropriate memory region for the current configuration.
 */

#include "ove/ove.h"
#include "FreeRTOS.h"
#include "task.h"
#include "serial_wrapper.h"
#include "stm32f7xx_hal.h"

#if configSUPPORT_DYNAMIC_ALLOCATION
/* FreeRTOS heap placement:
 * - SDRAM when inference is enabled (TFLM + models need significant RAM)
 * - .RamData2 (first 64 KB of SRAM1) when the heap fits
 * - Main RAM (.bss) otherwise
 * Not allocated at all in zero-heap mode (configSUPPORT_DYNAMIC_ALLOCATION=0). */
#if defined(CONFIG_OVE_INFER) || defined(CONFIG_OVE_LINUX)
/* Inference (TFLM models) and the Linux personality (a 2 MB program-region pool already in
 * SDRAM) both far exceed the 320 KB internal SRAM, so the FreeRTOS heap rides the external
 * 8 MB SDRAM too — otherwise a 128 KB ucHeap overflows the RAM region. */
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((aligned(8), section(".sdram_bss")));
#elif defined(HAL_ETH_MODULE_ENABLED) || configTOTAL_HEAP_SIZE > 0x10000
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

/* Diagnostic override for vApplicationStackOverflowHook: print the task
 * name via serial_safe_write (no FreeRTOS APIs allowed from the hook). */
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
