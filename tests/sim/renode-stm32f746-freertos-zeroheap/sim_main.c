/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Test runner for the Renode-hosted STM32F746 target.
 *
 * Mirrors tests/sim/freertos-qemu-zeroheap/sim_main.c.  The only
 * structural difference is the exception-handler set: the CubeF7 startup
 * assembly uses the full STM32F7 vector table, so SysTick / PendSV / SVC
 * are the only handlers we have to provide — the rest come out of the
 * FreeRTOS port or stay at Default_Handler.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "framework/ove_test.h"
#include "framework/semihosting_exit.h"

#include "stm32f7xx_hal.h"
#ifdef OVE_HW
#include "stm32746g_discovery_sdram.h"
extern SDRAM_HandleTypeDef sdramHandle; /* stm32746g_discovery_sdram.c */
#endif

#include <stdio.h>
#include <stdlib.h>

extern void xPortSysTickHandler(void);

/* Stub — tests exercise ove_app module without a real app entry point. */
void ove_main(void)
{
}

/* HAL_ETH_MspInit — production lives in
 * boards/stm32f746g-discovery/freertos/src/bus_msp_init.c, but that file
 * pulls in CONFIG_OVE_I2S/SAI dependencies we don't link.  Replicate the
 * Ethernet GPIO + RCC bring-up locally; Renode's SynopsysEthernetMAC
 * model accepts these without a real RMII PHY behind them. */
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

/* HAL_I2C_MspInit — the board's lives in bus_msp_init.c, which this image
 * does not link.  I2C3 (PH7 SCL, PH8 SDA) carries the FT5336 touch
 * controller the I2C tests talk to; Renode needs none of this, silicon
 * needs the clock and the pins. */
void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
	if (hi2c->Instance != I2C3)
		return;
	__HAL_RCC_I2C3_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();

	GPIO_InitTypeDef gpio = {0};
	gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8;
	gpio.Mode = GPIO_MODE_AF_OD;
	gpio.Pull = GPIO_PULLUP;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	gpio.Alternate = GPIO_AF4_I2C3;
	HAL_GPIO_Init(GPIOH, &gpio);
}

#ifdef CONFIG_OVE_I2C
#include "ove/i2c.h"

/* Results of I2C calls made before the scheduler starts, checked by
 * test_renode_i2c_before_scheduler: init, a register read, and a long
 * write-then-read.  In DTCM, as main RAM has no room left; so is the
 * handle's storage, which would not fit on main()'s small stack. */
__attribute__((section(".dsp_bss"))) int g_i2c_boot_rc[3];
__attribute__((section(".dsp_bss"))) static ove_i2c_storage_t boot_i2c_storage;

static void i2c_before_scheduler(void)
{
	static const uint8_t three[3] = {0xA4, 0x00, 0x00};
	const uint8_t reg = 0xA8;
	uint8_t rx[3] = {0};
	ove_i2c_t i2c = NULL;
	struct ove_i2c_cfg cfg = {
		.instance = 2,
		.speed = OVE_I2C_SPEED_STANDARD,
	};

	g_i2c_boot_rc[0] = ove_i2c_init(&i2c, &boot_i2c_storage, &cfg);
	if (g_i2c_boot_rc[0] != OVE_OK)
		return;
	g_i2c_boot_rc[1] = ove_i2c_write_read(i2c, 0x38, &reg, 1, rx, 1, OVE_MS(100));
	g_i2c_boot_rc[2] =
		ove_i2c_write_read(i2c, 0x38, three, sizeof(three), rx, sizeof(rx), OVE_MS(100));
	ove_i2c_deinit(i2c);
}
#endif

static void test_runner_task(void *arg)
{
	int failures = 0;
	(void)arg;

	/*
	 * Run the common CMocka suites under Renode's stm32f7_discovery-bb
	 * emulation.  STM32 HAL is linked in; the FreeRTOS ARM_CM7/r0p1
	 * port drives scheduling.  Some suites touch peripherals Renode
	 * doesn't model (IWDG, SAI); those self-gate via
	 * CONFIG_OVE_SKIP_RENODE_* or degrade gracefully on NOT_SUPPORTED.
	 */
#define OVE_SUITE(name, label)               \
	printf("=== " label " Tests ===\n"); \
	failures += test_##name##_run();
#include "framework/suites.inc"

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);

#ifdef OVE_HW
	/* Real silicon: the HW runner detects the summary line over USART1
	 * and ends the run.  semihosting_exit's bkpt #0xab would raise a
	 * HardFault here without a debugger attached, so just halt. */
	for (;;) {
	}
#else
	semihosting_exit(failures ? 1 : 0);
#endif
}

/* Static allocation callbacks required by configSUPPORT_STATIC_ALLOCATION */
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

/* Interrupt context for the tests' _from_isr cases (ove_test_run_in_isr(),
 * framework/ove_test_isr.h): the SysTick tick hook runs one queued call. */
void (*volatile ove_test_tick_fn)(void *);
void *volatile ove_test_tick_arg;

void vApplicationTickHook(void)
{
	void (*fn)(void *) = ove_test_tick_fn;
	if (fn) {
		ove_test_tick_fn = NULL;
		fn(ove_test_tick_arg);
	}
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

/* EXTI trampolines — the weak `Default_Handler` from the STM32 startup
 * is an infinite loop, so enabling an EXTI line with no override hangs
 * on the first pending edge.  HAL's `HAL_GPIO_EXTI_IRQHandler` clears
 * the EXTI pending bit and invokes `HAL_GPIO_EXTI_Callback`, which
 * `freertos_gpio.c` already implements.  Listing every EXTI line here
 * keeps the test firmware symmetric with what the board's main.c does
 * in production. */
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

/* Static storage for the test runner task */
static StaticTask_t runner_tcb;
static StackType_t runner_stack[configMINIMAL_STACK_SIZE * 8];

#ifdef OVE_HW
/* The zero-heap image keeps cmocka's malloc heap in the external SDRAM.
 * Renode models it as always present; on silicon the FMC must be brought
 * up first, as the production board's bsp_init does. This image runs on
 * the 16 MHz HSI rather than the board's 216 MHz, so the BSP's refresh
 * count (for a 100 MHz SDRAM clock) is reprogrammed for the real one:
 * 15.62 us per row, less the 20-cycle margin the reference manual asks. */
static void hw_sdram_init(void)
{
	(void)BSP_SDRAM_Init();
	/* One read-pipe cycle, as on the production board (bsp_sdram_fixup). */
	MODIFY_REG(FMC_Bank5_6->SDCR[0], FMC_SDCR1_RPIPE, FMC_SDCR1_RPIPE_0);
	uint32_t sdclk_hz = HAL_RCC_GetHCLKFreq() / 2u;
	uint32_t count = (uint32_t)(((uint64_t)sdclk_hz * 15620u) / 1000000000u) - 20u;
	(void)HAL_SDRAM_ProgramRefreshRate(&sdramHandle, count);
}
#endif

int main(void)
{
	/* HAL_Init configures SysTick via HAL_InitTick and installs the
	 * default prio.  Renode's RCC model accepts the clock-tree sequence
	 * without actually switching PLLs, so calls like HAL_RCC_OscConfig
	 * would hang spinning on HSI ready bits — we stick with HAL_Init
	 * only and let FreeRTOS's TICK_RATE_HZ drive the scheduler. */
	HAL_Init();
#ifdef OVE_HW
	hw_sdram_init();
#endif

	/* Enable every GPIO port clock so freertos_gpio.c's HAL_GPIO_Init /
	 * HAL_GPIO_WritePin calls reach the STM32_GPIOPort peripherals
	 * Renode models.  STM32F746 GPIOs A–I sit on AHB1. */
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOF_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();

	/* Peripheral clocks for the buses exercised by
	 * test_renode_stm32_periph.c.  Renode's STM32SPI / STM32F7_USART
	 * models accept register writes only when the corresponding RCC
	 * enable bit is set; the production board's MspInit handlers (in
	 * boards/stm32f746g-discovery/freertos/src/bus_msp_init.c) do this,
	 * but the Renode test target intentionally doesn't link that file
	 * (it routes pins for hardware MISO/MOSI/SCK we don't need under
	 * Renode's controller-level loopback). */
	__HAL_RCC_SPI1_CLK_ENABLE();

	/* Ethernet — Renode's SynopsysEthernetMAC needs the clock gate
	 * enabled before HAL_ETH_Init sees a working peripheral.  Pin
	 * configuration normally happens in HAL_ETH_MspInit (defined
	 * below); we enable the clock here for symmetry with the other
	 * peripheral classes. */
	__HAL_RCC_ETH_CLK_ENABLE();

	/* Enable the DWT cycle counter.  `freertos_time.c::ove_time_delay_us`
	 * spins on `DWT->CYCCNT`; without this initialisation the counter
	 * stays at 0 forever and `ove_time_delay_us` never returns under
	 * Renode.  Normally the board's bsp_init does this early — we
	 * replicate the sequence here because the Renode target builds
	 * without the board-specific bsp. */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	/* Cortex-M7: the DWT ignores writes until its lock is opened (as in
	 * freertos_time.c); Renode does not model the lock. */
	*((volatile uint32_t *)0xE0001FB0u) = 0xC5ACCE55u;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

#ifdef CONFIG_OVE_I2C
	i2c_before_scheduler(); /* before any task exists */
#endif
	xTaskCreateStatic(test_runner_task, "tests", configMINIMAL_STACK_SIZE * 8, NULL,
			  tskIDLE_PRIORITY + 1, runner_stack, &runner_tcb);
	vTaskStartScheduler();
	return 0;
}
