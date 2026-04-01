/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "bsp.h"
#include "stm32f7xx_hal.h"
//#include "stm32f429i_discovery.h"

static void prvSetupHardware(void);
static void SystemClock_Config(void);
static void prvErrorHandler(void);

int bsp_boardInit(void)
{
	prvSetupHardware();
	return (0);
}

void bsp_toggleLed(unsigned int led)
{
	if (led == 0) {
		//BSP_LED_Toggle(LED3);
	} else if (led == 1) {
		//BSP_LED_Toggle(LED4);
	} else {
		return;
	}
}

static void prvSetupHardware(void)
{
	/* Enable branch prediction */
	SCB->CCR |= (1 <<18);
	__DSB();

	/* Enable I-Cache */
	SCB_EnableICache();

	/* Enable D-Cache.  Disabled when Ethernet DMA is active — the ETH
	 * DMA reads/writes descriptors and buffers in SRAM that must be
	 * cache-coherent.  A proper fix would use MPU to mark the DMA
	 * region non-cacheable; for now we disable D-Cache entirely. */
#ifndef HAL_ETH_MODULE_ENABLED
	SCB_EnableDCache();
#endif

	HAL_Init();

	/* Configure the System clock to 200 MHz */
	SystemClock_Config();

//
//	BSP_LED_Init(LED3);
//	BSP_LED_Init(LED4);
//
//	BSP_LED_On(LED3);
//	BSP_LED_On(LED4);
}

/**
  * @brief  System Clock Configuration
  *         The system Clock is configured as follow :
  *            System Clock source            = PLL (HSE)
  *            SYSCLK(Hz)                     = 216000000
  *            HCLK(Hz)                       = 216000000
  *            AHB Prescaler                  = 1
  *            APB1 Prescaler                 = 4
  *            APB2 Prescaler                 = 2
  *            HSE Frequency(Hz)              = 25000000
  *            PLL_M                          = 25
  *            PLL_N                          = 432
  *            PLL_P                          = 2
  *            PLL_Q                          = 9
  *            VDD(V)                         = 3.3
  *            Main regulator output voltage  = Scale1 mode
  *            Flash Latency(WS)              = 7
  * @param  None
  * @retval None
  */
static void SystemClock_Config(void)
{
  RCC_ClkInitTypeDef RCC_ClkInitStruct;
  RCC_OscInitTypeDef RCC_OscInitStruct;
  HAL_StatusTypeDef ret = HAL_OK;

  /* Enable HSE Oscillator and activate PLL with HSE as source */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 432;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 9;

  ret = HAL_RCC_OscConfig(&RCC_OscInitStruct);
  if(ret != HAL_OK)
  {
    while(1) { ; }
  }

  /* Activate the OverDrive to reach the 216 MHz Frequency */
  ret = HAL_PWREx_EnableOverDrive();
  if(ret != HAL_OK)
  {
    while(1) { ; }
  }

  /* Select PLL as system clock source and configure the HCLK, PCLK1 and PCLK2 clocks dividers */
  RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  ret = HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7);
  if(ret != HAL_OK)
  {
    while(1) { ; }
  }
}

static void prvErrorHandler(void)
{
	while (1);
}
