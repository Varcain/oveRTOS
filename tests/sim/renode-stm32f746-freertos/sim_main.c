/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Test runner for the Renode-hosted STM32F746 target — heap mode.
 * Identical to the zero-heap sibling sim_main.c in structure; kept
 * separate because the CMake target name and ELF name differ.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "framework/ove_test.h"
#include "framework/semihosting_exit.h"

#include "stm32f7xx_hal.h"

#include <stdio.h>
#include <stdlib.h>

extern void xPortSysTickHandler(void);

void ove_main(void)
{
}

/* HAL_ETH_MspInit — see zero-heap sim_main.c for rationale. */
void HAL_ETH_MspInit(ETH_HandleTypeDef *h)
{
	(void)h;

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();

	GPIO_InitTypeDef gpio = {0};
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = GPIO_AF11_ETH;

	gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
	HAL_GPIO_Init(GPIOA, &gpio);

	gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
	HAL_GPIO_Init(GPIOC, &gpio);

	gpio.Pin = GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14;
	HAL_GPIO_Init(GPIOG, &gpio);
}

static void test_runner_task(void *arg)
{
	int failures = 0;
	(void)arg;

#define OVE_SUITE(name, label)               \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);

#ifdef OVE_HW
	/* See zero-heap sibling for rationale — bkpt #0xab faults on bare
	 * silicon without a debugger.  HW runner reads the summary line off
	 * USART1 and terminates the run from the host side. */
	for (;;) {
	}
#else
	semihosting_exit(failures ? 1 : 0);
#endif
}

/* Static allocation callbacks — still required because we enable
 * configSUPPORT_STATIC_ALLOCATION=1 alongside the heap. */
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
	fprintf(stderr, "\n!!! STACK OVERFLOW: %s !!!\n", pcTaskName ? pcTaskName : "(null)");
#ifdef OVE_HW
	for (;;) {
	}
#else
	semihosting_exit(1);
#endif
}

/* EXTI trampolines — see the zero-heap sibling sim_main.c for rationale. */
void EXTI0_IRQHandler(void)
{
	HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0);
}
void EXTI1_IRQHandler(void)
{
	HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_1);
}
void EXTI2_IRQHandler(void)
{
	HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_2);
}
void EXTI3_IRQHandler(void)
{
	HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_3);
}
void EXTI4_IRQHandler(void)
{
	HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_4);
}
void EXTI9_5_IRQHandler(void)
{
	for (unsigned int p = 5; p <= 9; ++p) {
		HAL_GPIO_EXTI_IRQHandler((uint16_t)(1U << p));
	}
}
void EXTI15_10_IRQHandler(void)
{
	for (unsigned int p = 10; p <= 15; ++p) {
		HAL_GPIO_EXTI_IRQHandler((uint16_t)(1U << p));
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

static StaticTask_t runner_tcb;
static StackType_t runner_stack[configMINIMAL_STACK_SIZE * 8];

int main(void)
{
	HAL_Init();

	/* Enable every GPIO port clock so freertos_gpio.c can reach the
	 * STM32_GPIOPort peripherals Renode models.  STM32F746 GPIOs A–I
	 * sit on AHB1. */
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOF_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();

	/* Peripheral clocks for buses exercised by test_renode_stm32_periph.c
	 * (matching the zero-heap variant — see that file for rationale). */
	__HAL_RCC_SPI1_CLK_ENABLE();

	/* Ethernet — see zero-heap variant for rationale. */
	__HAL_RCC_ETH_CLK_ENABLE();

	/* Enable the DWT cycle counter for freertos_time.c::ove_time_delay_us. */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	xTaskCreateStatic(test_runner_task, "tests", configMINIMAL_STACK_SIZE * 8, NULL,
			  tskIDLE_PRIORITY + 1, runner_stack, &runner_tcb);
	vTaskStartScheduler();
	return 0;
}
