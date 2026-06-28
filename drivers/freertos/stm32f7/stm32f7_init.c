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
#include "ove_config.h"

void stm32f7_mcu_init(void)
{
#if defined(CONFIG_OVE_LINUX)
	/* FreeRTOS-MPU (ARM_CM4_MPU) Linux-personality build: the scheduler's
	 * prvRestoreContextOfFirstTask resets the MSP from *VTOR.  SystemInit set
	 * VTOR = FLASH_BASE = 0x00200000 (the F7 ITCM flash alias), which is NOT covered by
	 * any FreeRTOS-MPU static region (the FLASH region maps the AXIM alias 0x08000000).
	 * After prvSetupMPU, a privileged read of the uncovered ITCM alias falls through to
	 * the background region and — with the M7 D-cache on — returns garbage, so the MSP
	 * is reset to garbage and the start-scheduler SVC bus-faults.  Re-point VTOR at the
	 * AXIM alias (same vector table, but MPU-covered + cacheable via the FLASH region). */
	SCB->VTOR = 0x08000000u;
	__DSB();
	__ISB();
#endif

	/* Enable branch prediction */
	SCB->CCR |= (1 << 18);
	__DSB();

	/* Enable I-Cache */
	SCB_EnableICache();

	/* Enable D-Cache.  Disabled when Ethernet DMA is active — the ETH
	 * DMA reads/writes descriptors and buffers in SRAM that must be
	 * cache-coherent.  A proper fix would use MPU to mark the DMA
	 * region non-cacheable; for now we disable D-Cache entirely.
	 *
	 * ALSO disabled for the FreeRTOS-MPU (ARM_CM4_MPU) Linux-personality build: that port
	 * reads task contexts/stacks through MPU static regions whose cacheability differs from
	 * how they were written, and (unlike the non-MPU CM7 build) it does no SCB cache
	 * maintenance at the scheduler start / context switch → a stale D-cache read faults
	 * vSVCHandler_C at the start-scheduler SVC.  Keep the D-cache off (the SDRAM program
	 * pool is already Normal non-cacheable); a finer fix is cacheable-coherent MPU attrs. */
#if !defined(HAL_ETH_MODULE_ENABLED) && !defined(CONFIG_OVE_LINUX)
	SCB_EnableDCache();
#endif

	HAL_Init();
}
