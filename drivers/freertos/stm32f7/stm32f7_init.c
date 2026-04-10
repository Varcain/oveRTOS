/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Cortex-M7 / STM32F7 MCU-level initialisation.
 * Reusable across any STM32F7 board — no board-specific content.
 */

#include "stm32f7_init.h"
#include "stm32f7xx_hal.h"

void stm32f7_mcu_init(void)
{
	/* Enable branch prediction */
	SCB->CCR |= (1 << 18);
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
}
