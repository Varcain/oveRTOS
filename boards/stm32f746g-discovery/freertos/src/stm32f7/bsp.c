/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746G-Discovery board support — clock configuration and board init.
 */

#include "bsp.h"
#include "stm32f7xx_hal.h"
#include "stm32f7_init.h"

static void SystemClock_Config(void);
static void MPU_Config_SDRAM(void);

int bsp_boardInit(void)
{
	/* Remap external SDRAM (0xC0000000, 8 MB) from the default ARM
	 * Device-memory attributes to Normal non-cacheable.  Without this,
	 * any unaligned access into SDRAM (compiler-emitted strh/strd to
	 * an odd stack offset, toolchain memcpy widening to 4-byte stores,
	 * etc.) raises UFSR.UNALIGNED → HardFault.  Heap_4 ucHeap lives in
	 * .sdram_bss when CONFIG_OVE_INFER=y, so every heap-allocated task
	 * stack would otherwise fault on its first non-trivial stack frame. */
	MPU_Config_SDRAM();

	/* MCU-level init (cache, branch prediction, HAL_Init) */
	stm32f7_mcu_init();

	/* Board-specific clock: 25 MHz HSE → 216 MHz SYSCLK */
	SystemClock_Config();

	return 0;
}

static void MPU_Config_SDRAM(void)
{
	MPU_Region_InitTypeDef mpu = {0};

	HAL_MPU_Disable();

	mpu.Enable = MPU_REGION_ENABLE;
	mpu.Number = MPU_REGION_NUMBER0;
	mpu.BaseAddress = 0xC0000000;
	mpu.Size = MPU_REGION_SIZE_8MB;
	mpu.SubRegionDisable = 0x00;
	/* Normal, non-cacheable, non-shareable.  Non-cacheable keeps LTDC
	 * framebuffer reads automatically coherent with CPU writes — no
	 * SCB_CleanDCache calls needed.  Switch to write-back cacheable
	 * (TEX=0, C=1, B=1) once the framebuffer is carved out into its
	 * own non-cacheable sub-region. */
	mpu.TypeExtField = MPU_TEX_LEVEL1;
	mpu.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
	mpu.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
	mpu.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
	mpu.AccessPermission = MPU_REGION_FULL_ACCESS;
	mpu.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
	HAL_MPU_ConfigRegion(&mpu);

	HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void bsp_toggleLed(unsigned int led)
{
	(void)led;
}

/**
  * System Clock Configuration
  *   System Clock source            = PLL (HSE)
  *   SYSCLK(Hz)                     = 216000000
  *   HCLK(Hz)                       = 216000000
  *   AHB Prescaler                  = 1
  *   APB1 Prescaler                 = 4
  *   APB2 Prescaler                 = 2
  *   HSE Frequency(Hz)              = 25000000
  *   PLL_M                          = 25
  *   PLL_N                          = 432
  *   PLL_P                          = 2
  *   PLL_Q                          = 9
  */
static void SystemClock_Config(void)
{
	RCC_ClkInitTypeDef RCC_ClkInitStruct;
	RCC_OscInitTypeDef RCC_OscInitStruct;

	/* Enable HSE Oscillator and activate PLL with HSE as source */
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	RCC_OscInitStruct.HSEState = RCC_HSE_ON;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	RCC_OscInitStruct.PLL.PLLM = 25;
	RCC_OscInitStruct.PLL.PLLN = 432;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = 9;

	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
		while (1)
			;

	/* Activate the OverDrive to reach 216 MHz */
	if (HAL_PWREx_EnableOverDrive() != HAL_OK)
		while (1)
			;

	/* Configure HCLK, PCLK1 and PCLK2 dividers */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
				      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK)
		while (1)
			;
}
