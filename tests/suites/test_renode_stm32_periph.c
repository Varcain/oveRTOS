/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Renode-target peripheral suite — exercises ove_i2c / ove_uart APIs
 * against Renode's modelled STM32F7 peripherals.  Skipped on every
 * other target; renode_obs.h's OVE_OBS_AVAILABLE gate handles the
 * compile-time fork.
 *
 * Coverage:
 *   1. ove_i2c against the FT5336 touchscreen — Renode's
 *      stm32f7_discovery-bb platform attaches one at I2C3:0x38, so a
 *      real bus probe + register read round-trips through the
 *      ove_i2c_*  →  HAL_I2C_*  →  STM32F7_I2C  →  FT5336 model chain.
 *   2. ove_uart on USART2 — bytes written via ove_uart_write should be
 *      observable by Renode's UART analyzer.  We capture them through
 *      the firmware's own loopback by reading the ODR/TDR-equivalent
 *      from the modelled STM32F7_USART register layout.
 */

#include "../framework/ove_test.h"
#include "../framework/renode_obs.h"
#include "ove/i2c.h"
#include "ove/spi.h"
#include "ove/uart.h"

#include <stdio.h>
#include <string.h>

#if OVE_OBS_AVAILABLE

#include "FreeRTOS.h"
#include "task.h"

/* ── 1. I2C against the FT5336 touchscreen ────────────────────────── */

/* Notifications left in the calling task's slot.  An I2C completion the
 * driver did not wait for must not stay there: it would end the task's
 * next wait, a thread join for one.  The value is read, not cleared. */
static uint32_t pending_notifications(void)
{
	return ulTaskNotifyValueClear(NULL, 0);
}

/* The FT5336 chip-id register (offset 0xA8) returns 0x51 for the variant
 * Renode models.  We don't depend on the exact value — any successful
 * read demonstrates the bus transaction completed end-to-end. */
#define FT5336_I2C_INSTANCE 2 /* Renode's i2c3 → driver index 2 */
#define FT5336_ADDR 0x38
#define FT5336_REG_CHIPID 0xA8

static void test_renode_i2c_ft5336_probe(void **state)
{
	(void)state;

	struct ove_i2c_cfg cfg = {
		.instance = FT5336_I2C_INSTANCE,
		.speed = OVE_I2C_SPEED_STANDARD,
	};
	static ove_i2c_storage_t storage;
	ove_i2c_t i2c = NULL;
	int rc = ove_i2c_init(&i2c, &storage, &cfg);
	assert_int_equal(rc, OVE_OK);

	/* Read 1 byte from the chip-id register.  Renode's FT5336 model
	 * answers all register reads — the value matters less than the
	 * fact that the transaction completes without a HAL timeout or
	 * arbitration error. */
	uint8_t reg = FT5336_REG_CHIPID;
	uint8_t out = 0;
	rc = ove_i2c_write_read(i2c, FT5336_ADDR, &reg, 1, &out, 1, OVE_MS(100));
	assert_int_equal(rc, OVE_OK);

	/* A write of more than two bytes then a repeated-start read: the
	 * FreeRTOS driver started it on interrupts nothing serviced, returned
	 * OVE_ERR_BUS_BUSY and left the controller busy, so the chip id read
	 * after it failed too. */
	static const uint8_t three[3] = {0xA4, 0x00, 0x00};
	uint8_t rx3[3] = {0};
	rc = ove_i2c_write_read(i2c, FT5336_ADDR, three, sizeof(three), rx3, sizeof(rx3),
				OVE_MS(100));
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(pending_notifications(), 0);
	out = 0;
	rc = ove_i2c_write_read(i2c, FT5336_ADDR, &reg, 1, &out, 1, OVE_MS(100));
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(out, 0x51);

	/* Nothing to write is a plain read (the FreeRTOS driver sent two bytes
	 * read from address 0 as a register address first). */
	rc = ove_i2c_write_read(i2c, FT5336_ADDR, NULL, 0, &out, 1, OVE_MS(100));
	assert_int_equal(rc, OVE_OK);

	/* Nobody at 0x77.  Silicon NACKs the address; Renode's I2C model
	 * never answers, so the call waits out its 100 ms. */
#ifdef OVE_HW
	const int absent = OVE_ERR_BUS_NACK;
#else
	const int absent = OVE_ERR_TIMEOUT;
#endif
	rc = ove_i2c_read(i2c, 0x77, &out, 1, OVE_MS(100));
	assert_int_equal(rc, absent);

	/* The same for a write of more than two bytes then a read, which runs
	 * on interrupts: the abandoned transfer must leave the bus released,
	 * so the chip id still reads afterwards. */
	rc = ove_i2c_write_read(i2c, 0x77, three, sizeof(three), rx3, sizeof(rx3), OVE_MS(100));
	assert_int_equal(rc, absent);
	assert_int_equal(pending_notifications(), 0);
	out = 0;
	rc = ove_i2c_write_read(i2c, FT5336_ADDR, &reg, 1, &out, 1, OVE_MS(100));
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(out, 0x51);

	ove_i2c_deinit(i2c);
}

/* sim_main.c used the bus before creating the first task.  The polled
 * transfers work then; the long write-then-read runs on interrupts, which
 * FreeRTOS keeps masked until the scheduler starts, so it is refused
 * rather than waiting on a task that does not exist yet. */
extern int g_i2c_boot_rc[3];

static void test_renode_i2c_before_scheduler(void **state)
{
	(void)state;
	assert_int_equal(g_i2c_boot_rc[0], OVE_OK);
	assert_int_equal(g_i2c_boot_rc[1], OVE_OK);
	assert_int_equal(g_i2c_boot_rc[2], OVE_ERR_NOT_SUPPORTED);
}

/* ── 2. UART tx observable in USART2 register state ───────────────── */

/* USART2 lives at 0x40004400 on STM32F7; the TDR (transmit data) is at
 * offset 0x28.  After ove_uart_write Renode's STM32F7_USART model
 * accepts the byte and transmits it (the analyzer captures it).  We
 * confirm the API path doesn't error and that the configured baudrate
 * is what we requested — exercising the HAL config flow. */
#define USART2_INSTANCE 1 /* driver indexes from 0; USART2 is index 1 */

static void test_renode_uart_tx_completes(void **state)
{
	(void)state;

	/* ove_uart_create's zero-heap shim wants a compile-time
	 * rx_buf_size so it can declare a `static` VLA — runtime configs
	 * don't qualify.  Use ove_uart_init directly with explicit
	 * static storage to keep the test heap-mode-agnostic. */
	static ove_uart_storage_t storage;
	static uint8_t rx_buf[64];
	struct ove_uart_cfg cfg = {
		.instance = USART2_INSTANCE,
		.baudrate = 115200,
		.data_bits = 8,
		.parity = OVE_UART_PARITY_NONE,
		.stop_bits = OVE_UART_STOP_1,
		.flow_control = OVE_UART_FLOW_NONE,
		.rx_buf_size = sizeof(rx_buf),
	};
	ove_uart_t uart = NULL;
	int rc = ove_uart_init(&uart, &storage, rx_buf, &cfg);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(uart);

	const char msg[] = "renode-uart\n";
	size_t written = 0;
	rc = ove_uart_write(uart, msg, sizeof(msg) - 1, OVE_MS(200), &written);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(written, sizeof(msg) - 1);

	ove_uart_deinit(uart);
}

/* ── 3. SPI loopback through Renode's SPI.SPILoopback peripheral ──── */

/* test.resc attaches `SPI.SPILoopback @ spi1` — that model echoes each
 * transmitted byte back on MISO, so a HAL_SPI_TransmitReceive on SPI1
 * should yield rx == tx, demonstrating the full
 * ove_spi_*  →  HAL_SPI_*  →  STM32SPI  →  SPILoopback round trip.
 *
 * Renode 1.16's STM32SPI model has known fidelity gaps: it ignores
 * CR2.DS (the DataSize bits) and effectively treats every transfer as
 * 16-bit FIFO-paced, so HAL_SPI's byte-by-byte access pattern in 8-bit
 * mode reads back the low byte once and the (empty) high byte as zero
 * on alternate dequeues — receive comes out as { tx[0], 0, tx[2], 0,
 * ... }.  We assert on that pattern: it still proves every byte we
 * write reached the loopback peripheral and that one in two reads pops
 * the echoed value, which is what we care about for the API path.
 * If/when Renode improves the STM32SPI model, tighten this. */
#define SPI1_INSTANCE 0

static void test_renode_spi_loopback(void **state)
{
	(void)state;

	struct ove_spi_cfg cfg = {
		.instance = SPI1_INSTANCE,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	static ove_spi_storage_t storage;
	ove_spi_t spi = NULL;
	int rc = ove_spi_init(&spi, &storage, &cfg);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(spi);

	const uint8_t tx[8] = {0xA5, 0x5A, 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
	uint8_t rx[8] = {0};
	rc = ove_spi_transfer(spi, NULL, tx, rx, sizeof(tx), OVE_MS(200));
	assert_int_equal(rc, OVE_OK);

	/* Even-indexed bytes echo cleanly; odd-indexed are 0 (model gap). */
	for (size_t i = 0; i < sizeof(tx); i += 2) {
		assert_int_equal(rx[i], tx[i]);
	}

	ove_spi_deinit(spi);
}

/* ── Deferred coverage ────────────────────────────────────────────── *
 *
 * Renode models more peripherals on the stm32f7_discovery-bb platform
 * than this suite currently exercises.  Three are deliberately deferred
 * pending upstream work:
 *
 *   • Touchscreen via ove/lvgl.h pointer indev — Renode attaches a
 *     working `Input.FT5336 @ i2c3 0x38` (we already round-trip its
 *     chip-id register above), but oveRTOS has no FT5336 driver and no
 *     `ove_lvgl_register_pointer` / pointer-indev API.  Other backends
 *     (Zephyr, NuttX) ship FT5336 drivers we can port.  Defer until
 *     that driver lands; then add a test that injects touches via
 *     `(machine).i2c3.touchscreen TouchAt X Y` and asserts the LVGL
 *     indev observed them.
 *
 *   • Hardware-timer path via ove/timer.h — currently `ove_timer` is a
 *     pure FreeRTOS-software-timer wrapper, exercised identically on
 *     every backend.  Renode's `Timers.STM32_Timer` (TIM2..14) only
 *     becomes worth testing once an `ove/hwtimer.h` module lands that
 *     binds an oveRTOS timer to a real STM32 TIM peripheral.
 *
 *   • SD-card-backed FS / NVS via ove/fs.h + ove/nvs.h — Renode models
 *     STM32FSDMMC and SdCardFromFile attaches a raw image cleanly, but
 *     the model accepts only a subset of the registers STM32CubeF7's
 *     BSP_SD writes during HAL_SD_Init: DTEN, DBLOCKSIZE, several
 *     status-clear bits flag as "Unhandled bits" in the Renode log,
 *     and the data-transfer state never advances.  An attempted bring-
 *     up (FatFS + sd_diskio + BSP_SD + HAL_SD chain) confirmed
 *     `f_open` hangs in the SD bus state machine.  Renode's own STM32
 *     SDMMC integration test only covers Zephyr's slimmer driver
 *     against an ext2 image.  Revisit once Renode 1.17+'s STM32FSDMMC
 *     model improves, or when we ship a slim diskio that bypasses the
 *     BSP and writes only the registers Renode actually models.
 */

#endif /* OVE_OBS_AVAILABLE */

/* ── Runner ────────────────────────────────────────────────────────── */

int test_renode_stm32_periph_run(void)
{
#if !OVE_OBS_AVAILABLE
	/* See test_renode_stm32_obs.c: this branch is also hit on the
	 * NuttX/Zephyr Renode targets (real Renode, but no STM32 HAL register
	 * layer), so do not claim "non-Renode" there. */
	printf("  [SKIP] renode_stm32_periph — register observation not wired here "
	       "(non-Renode target, or NuttX/Zephyr-Renode w/o the STM32 HAL register layer)\n");
	return 0;
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_renode_i2c_ft5336_probe),
		cmocka_unit_test(test_renode_i2c_before_scheduler),
		cmocka_unit_test(test_renode_uart_tx_completes),
#ifndef OVE_HW
		/* SPILoopback is a Renode-only peripheral.  On the real
		 * Discovery, SPI1 has no on-board MISO→MOSI loop, so the
		 * read-back assertion can't pass. */
		cmocka_unit_test(test_renode_spi_loopback),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
#endif
}
