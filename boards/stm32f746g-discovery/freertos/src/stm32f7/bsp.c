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
#include "ove/reset.h" /* ove_reset_cause — reads RCC->CSR when CONFIG_OVE_WATCHDOG */
#if defined(CONFIG_OVE_QSPI)
#include "stm32746g_discovery_qspi.h" /* BSP_QSPI_Init / EnableMemoryMappedMode (N25Q128A) */
#endif

#if defined(CONFIG_OVE_WATCHDOG)
/* Latched once at board init (RCC->CSR reset flags survive across the reset that
 * set them but persist until cleared, so they must be read before a later boot
 * ORs its own cause in). ove_reset_cause() returns this for the whole boot. */
static ove_reset_cause_t g_reset_cause = OVE_RESET_UNKNOWN;

static void bsp_latch_reset_cause(void)
{
	/* Most-specific cause first: a cold power-on also asserts PIN (and BOR) on
	 * this part, so those are only reached when nothing more specific is set —
	 * and IWDG/WWDG win over everything so a watchdog recovery is never masked. */
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) || __HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST))
		g_reset_cause = OVE_RESET_WATCHDOG;
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))
		g_reset_cause = OVE_RESET_SOFTWARE;
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST))
		g_reset_cause = OVE_RESET_LOW_POWER;
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))
		g_reset_cause = OVE_RESET_POWER_ON;
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))
		g_reset_cause = OVE_RESET_BROWNOUT;
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))
		g_reset_cause = OVE_RESET_PIN;
	__HAL_RCC_CLEAR_RESET_FLAGS();
}

ove_reset_cause_t ove_reset_cause(void)
{
	return g_reset_cause;
}
#endif /* CONFIG_OVE_WATCHDOG */

static void SystemClock_Config(void);
static void MPU_Config_SDRAM(void);
static int sdram_selftest(void);
#if defined(CONFIG_OVE_QSPI)
static void bsp_qspi_init(void);
#endif

#if defined(CONFIG_OVE_STACK_CANARIES)
/* The -fstack-protector-strong canary base is defined and default-initialised by picolibc; a guest
 * cannot reach it (host memory behind the MPU), so the only thing to add is unpredictability. Seed
 * it from the hardware RNG at boot, before any guest runs. picolibc's default covers only the
 * pre-seed boot window. */
extern uintptr_t __stack_chk_guard;

/* Reseed. no_stack_protector, and it fills a LOCAL then assigns: changing __stack_chk_guard while a
 * function whose canary was set from the OLD value is still live would fail that function's exit
 * check. bsp_random_fill writes the local (its canary stays consistent); this function has no
 * canary; and bsp_boardInit (the only caller here) is no_stack_protector too, so nothing straddles
 * the change. Functions called before use the old base and have already returned; those after use
 * the new one. */
__attribute__((no_stack_protector)) static void stack_guard_seed(void)
{
	uintptr_t g;
	if (bsp_random_fill(&g, sizeof g) == OVE_OK)
		__stack_chk_guard = g;
}
#define BSP_BOARDINIT_ATTR __attribute__((no_stack_protector))
#else
#define BSP_BOARDINIT_ATTR
#endif

BSP_BOARDINIT_ATTR int bsp_boardInit(void)
{
#if defined(CONFIG_OVE_WATCHDOG)
	/* Before anything else, so the reset flags reflect the reset that started
	 * THIS boot (nothing here clears RCC->CSR, but read-then-clear early leaves
	 * no doubt). Reads fine without any clock config — RCC is always clocked. */
	bsp_latch_reset_cause();
#endif

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

#if defined(CONFIG_OVE_WATCHDOG)
	/* Freeze the IWDG counter whenever the core is halted by a debugger, so
	 * openocd can halt-and-flash a board whose watchdog is already armed without
	 * the IWDG resetting it mid-operation. Debug-domain bit; a running system is
	 * unaffected (the counter runs normally when the core is not halted). */
	__HAL_DBGMCU_FREEZE_IWDG();
#endif

	/* Board-specific clock: 25 MHz HSE → 216 MHz SYSCLK */
	SystemClock_Config();

	/* The guest entropy source uses PLL48CLK (432 MHz / PLLQ=9 = 48 MHz).
	 * Failure is deliberately non-fatal to the host RTOS: guest process launch
	 * and random syscalls fail closed through the LXP port instead. */
	(void)bsp_random_init();

#if defined(CONFIG_OVE_STACK_CANARIES)
	/* Right after the RNG is up (and long before any guest), so the stack-protector canary is
	 * unpredictable for every guest-input path that runs later. */
	stack_guard_seed();
#endif

	/* Bring up the FMC controller + the external 8 MB SDRAM at 0xC0000000.  On QEMU/Renode
	 * the SDRAM is modeled as always-present so this step was never needed; on real silicon
	 * the controller MUST be initialized or the first access to 0xC0000000 faults.  The Linux
	 * personality's program/dynamic pools and cold coordinator storage live here, so this
	 * runs at board init before anything touches them.  Validate the array before trusting it. */
	(void)BSP_SDRAM_Init();
	bsp_sdram_fixup();
	if (sdram_selftest() != 0)
		for (;;) { /* SDRAM read/write/verify failed — halt here (GDB-findable) */
		}

#if defined(CONFIG_OVE_QSPI)
	bsp_qspi_init();
#endif

	return 0;
}

#if defined(CONFIG_OVE_QSPI)
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
	/* Run the memory-mapped QSPI XIP read clock at 216/(1+1) = 108 MHz — the N25Q128A's rated
	 * quad-read ceiling (with the 10 dummy cycles + half-cycle sample shift BSP_QSPI_Init sets)
	 * and the STM32Cube Discovery BSP default.  The FDPIC guest XIPs its code + rodata from here,
	 * so this clock directly gates render throughput: 108 MHz vs the earlier conservative 54 MHz
	 * lifts lvbench 16->22 FPS, validated 5/5 clean runs on silicon. */
	MODIFY_REG(QUADSPI->CR, QUADSPI_CR_PRESCALER, (1u << QUADSPI_CR_PRESCALER_Pos));
	if (BSP_QSPI_EnableMemoryMappedMode() != QSPI_OK)
		for (;;) { /* memory-mapped enable failed — halt (GDB-findable) */
		}
}
#endif

/* Add one FMC read-data pipe-delay cycle (SDCR.RPIPE = 1) for reliable SDRAM
 * reads at the 108 MHz FMC clock under LTDC contention.  The Cube Discovery BSP
 * programs RPIPE=0 (no delay).  With the Linux personality that alone is fine —
 * until the fb backend turns the LTDC on: the LTDC then continuously burst-reads
 * the framebuffer out of the SAME SDRAM, and those bursts shift the bus timing
 * enough that the CPU's read-data capture goes marginal at 108 MHz → SDRAM reads
 * take bit-flips → a corrupted lv_obj class pointer crashes LVGL's event dispatch.
 * (Originally diagnosed with the guest pool mapped uncached, so every heap access
 * was a real SDRAM read; the pool is now WBWA-cached by LXP's FreeRTOS port, but
 * D-cache line fills are still SDRAM bursts, so the fix stays.)  One read-pipe
 * cycle restores the
 * capture margin.  Verified on silicon: lvbench renders to completion with the
 * LTDC scanning the panel only with RPIPE>=1 (AC timing + refresh were ruled out
 * — neither relaxing SDTR nor doubling the refresh rate helped).  SDCR is honored
 * live (no re-init needed), but BSP_SDRAM_Init resets it, so this must be
 * re-applied after anything that re-runs it — notably BSP_LCD_Init. */
void bsp_sdram_fixup(void)
{
	MODIFY_REG(FMC_Bank5_6->SDCR[0], FMC_SDCR1_RPIPE, FMC_SDCR1_RPIPE_0);
}

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
	/* Temporary pre-scheduler view: Normal, non-cacheable, non-shareable. It lets board init and
	 * the SDRAM self-test use ordinary unaligned accesses before FreeRTOS starts. The ARM_CM4_MPU
	 * port replaces the hardware MPU setup at scheduler start; restricted Linux guests then receive
	 * cacheable, per-task SDRAM regions from freertos_spawn_common(). Execute permission here does
	 * not grant guest execution and is retained for non-personality pre-scheduler users. */
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
