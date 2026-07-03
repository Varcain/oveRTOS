/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746G-Discovery board support — clock configuration and board init.
 */

#include "ove_config.h"

#include "bsp.h"
#include "stm32f7xx_hal.h"
#include "stm32746g_discovery_sdram.h" /* BSP_SDRAM_Init — bring up the FMC + external SDRAM */
#include "stm32f7_init.h"
#if defined(CONFIG_OVE_QSPI)
#include "stm32746g_discovery_qspi.h" /* BSP_QSPI_Init / EnableMemoryMappedMode (N25Q128A) */
#endif

static void SystemClock_Config(void);
static void MPU_Config_SDRAM(void);
static int sdram_selftest(void);
#if defined(CONFIG_OVE_QSPI)
static void bsp_qspi_init(void);
#endif

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

	/* Bring up the FMC controller + the external 8 MB SDRAM at 0xC0000000.  On QEMU/Renode
	 * the SDRAM is modeled as always-present so this step was never needed; on real silicon
	 * the controller MUST be initialized or the first access to 0xC0000000 faults.  The Linux
	 * personality's 2 MB program-region pool + 1 MB dyn pools live here (.sdram_bss), so this
	 * runs at board init before anything touches them.  Validate the array before trusting it. */
	(void)BSP_SDRAM_Init();
	if (sdram_selftest() != 0)
		for (;;) { /* SDRAM read/write/verify failed — halt here (GDB-findable) */
		}

#if defined(CONFIG_OVE_QSPI)
	bsp_qspi_init();
#endif

	return 0;
}

#if defined(CONFIG_OVE_QSPI)
/* SDRAM-staged QSPI programming for flash-qspi.sh.  A host debugger (openocd)
 * halts at bsp_qspi_flash_stage after the SDRAM + QUADSPI (indirect) are up,
 * loads the rootfs.cpio into SDRAM at QSPI_STAGE_DATA + a {magic, len} header at
 * QSPI_STAGE_HDR, and resumes; we erase + program the NOR from SDRAM at QUADSPI
 * speed (far faster than programming over SWD), then set the magic to DONE.  A
 * normal boot finds no request (the magic won't match uninitialised SDRAM) and
 * returns at once — no delay, no side effects. */
#define QSPI_STAGE_HDR	((volatile uint32_t *)0xC01F0000u)
#define QSPI_STAGE_DATA ((const uint8_t *)0xC0200000u)
#define QSPI_STAGE_REQ	0x51535052u /* 'QSPR' — host requests a program */
#define QSPI_STAGE_DONE 0x444F4E45u /* 'DONE' — target finished */

/* Non-static + noinline so flash-qspi.sh can set a breakpoint here (the host
 * stages the cpio + header while halted at the top of this function). */
__attribute__((noinline)) void bsp_qspi_flash_stage(void)
{
	if (QSPI_STAGE_HDR[0] != QSPI_STAGE_REQ)
		return;
	uint32_t len = QSPI_STAGE_HDR[1];
	if (len == 0u || len > N25Q128A_FLASH_SIZE)
		return;
	/* BSP_QSPI_Erase_Block issues SUBSECTOR_ERASE_CMD — a 4 KB erase, despite the
	 * "Block" name — so it must be called per N25Q128A_SUBSECTOR_SIZE (0x1000).  A
	 * 64 KB stride would leave 60 KB of every 64 KB unerased, and the page writes
	 * would then AND into the stale contents (verified on silicon: OK in each
	 * subsector's first 4 KB, garbage after). */
	for (uint32_t a = 0; a < len; a += N25Q128A_SUBSECTOR_SIZE)
		BSP_QSPI_Erase_Block(a);
	for (uint32_t off = 0; off < len; off += 256u) { /* page program (quad, BSP) */
		uint32_t n = (len - off < 256u) ? (len - off) : 256u;
		BSP_QSPI_Write((uint8_t *)(QSPI_STAGE_DATA + off), off, n);
	}
	QSPI_STAGE_HDR[0] = QSPI_STAGE_DONE;
}

/* Bring up the on-board QSPI NOR (N25Q128A, 16 MB) in memory-mapped mode, so
 * external flash is CPU-addressable + executable at 0x90000000 before anything
 * (e.g. the Linux personality rootfs XIP) reads it.  Runs at board init, before
 * the scheduler / app tasks.  Under the ARM default map 0x90000000 is Normal
 * executable memory, so the privileged personality reaches it via PRIVDEFENA;
 * the unprivileged guest gets a dedicated per-task RO+X MPU region for the QSPI
 * window (freertos_spawn_common, CONFIG_OVE_LINUX_ROOTFS_QSPI). */
static void bsp_qspi_init(void)
{
	if (BSP_QSPI_Init() != QSPI_OK)
		for (;;) { /* QSPI controller/chip bring-up failed — halt (GDB-findable) */
		}
	/* The Cube BSP clocks the QUADSPI at 216/(1+1) = 108 MHz.  Drop to 216/(3+1) =
	 * 54 MHz — well within the N25Q128A's rating — as a conservative margin for the
	 * memory-mapped quad read the guest XIPs from.  Safe to change while idle
	 * (BUSY=0 right after init). */
	MODIFY_REG(QUADSPI->CR, QUADSPI_CR_PRESCALER, (3u << QUADSPI_CR_PRESCALER_Pos));
	bsp_qspi_flash_stage(); /* host-assisted programming (indirect mode) — no-op on a normal boot */
	if (BSP_QSPI_EnableMemoryMappedMode() != QSPI_OK)
		for (;;) { /* memory-mapped enable failed — halt (GDB-findable) */
		}
}
#endif

/* Walk the 8 MB SDRAM at a coarse stride writing an address-dependent pattern, then read it
 * back — catches a dead controller, a wrong refresh rate, or address-line aliasing before the
 * personality relies on the region pool living there.  Returns 0 on success, -1 on mismatch. */
static int sdram_selftest(void)
{
	volatile uint32_t *sdram = (volatile uint32_t *)0xC0000000u;
	const uint32_t span = 8u * 1024u * 1024u;
	const uint32_t stride = 0x10000u; /* 64 KB */
	for (uint32_t off = 0; off < span; off += stride)
		sdram[off / 4u] = 0xA5A50000u ^ off;
	for (uint32_t off = 0; off < span; off += stride)
		if (sdram[off / 4u] != (0xA5A50000u ^ off))
			return -1;
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
	/* Normal, non-cacheable, non-shareable, EXECUTABLE.  Non-cacheable keeps the
	 * LTDC framebuffer — and, importantly, the Linux personality's loaded and
	 * relocated program code — coherent with CPU writes without any
	 * SCB_CleanDCache/InvalidateICache maintenance on the M7.  Execution is
	 * ENABLED because a NOMMU personality process runs its bFLT/FDPIC code
	 * in-place from a program region that lives in this SDRAM (.sdram_bss);
	 * leaving the region XN would MemManage-fault on the program's first fetch. */
	mpu.TypeExtField = MPU_TEX_LEVEL1;
	mpu.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
	mpu.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
	mpu.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
	mpu.AccessPermission = MPU_REGION_FULL_ACCESS;
	mpu.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
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
