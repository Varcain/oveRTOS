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
	 * region non-cacheable; for now we disable D-Cache entirely. */
#if defined(CONFIG_OVE_LINUX) && defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	/* Linux personality on the real STM32F746: the D-cache runs FULLY ENABLED in its default
	 * write-back mode (the lvbench compositing win + full SRAM performance — no global
	 * write-through compromise).  The guest XIPs its FDPIC text and the loader/rootfs parse read
	 * the CPIO from the QUADSPI-mapped NOR at 0x90000000, where naive D-cached memory-mapped reads
	 * corrupt.  That hazard is fixed WHERE IT BELONGS — at the MPU-region level, per accessing
	 * context — not by detuning the whole cache:
	 *   - every context reads the QUADSPI through a region sized to exactly the 16 MB N25Q128A,
	 *     so the M7 cannot speculatively prefetch past the chip into unmapped QUADSPI space;
	 *   - the privileged coordinator/loader reads it through a NON-cacheable per-task region
	 *     (ove_lnx_rootfs_window, backends/freertos/freertos_lnx.c), so the cache never issues a
	 *     line-fill burst to the memory-mapped QUADSPI (the decisive factor: on silicon an NC
	 *     bounded region reads the NOR reliably where a cacheable/write-through one does not);
	 *   - the unprivileged guest keeps a cacheable bounded region (freertos_spawn_common) for
	 *     fast in-place XIP, and creates no dirty write-back lines (its RW data is non-cacheable
	 *     SDRAM), so it never triggers the burst-collision path either. */
	SCB_EnableDCache();
#elif !defined(HAL_ETH_MODULE_ENABLED) && !defined(CONFIG_OVE_LINUX)
	SCB_EnableDCache();
#endif

	HAL_Init();
}
