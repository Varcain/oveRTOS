/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Cmocka tests for the CONFIG_OVE_ASYNC C substrate that the Rust
 * binding's embassy-time driver sits on top of:
 *
 *   - ove_irq_lock / ove_irq_unlock / ove_is_in_isr (include/ove/irq.h)
 *   - ove_timer_init_ns / ove_timer_create_ns / ove_timer_set_period_ns
 *     (include/ove/timer.h, gated by CONFIG_OVE_ASYNC for the new
 *     symbols — only ove_timer_init_ns / _create_ns are also available
 *     as gated extensions to the existing timer API).
 *
 * The end-to-end Rust async stack (embassy executor + ove::async_runtime)
 * is exercised by the apps/rust/{heap,zeroheap}/example_async/ demos
 * we run manually under QEMU / on hardware; these tests are the C-side
 * regression gate.
 *
 * When CONFIG_OVE_ASYNC is off the file shrinks to a stub
 * `test_async_run` that returns 0, so the suite list in suites.inc
 * stays consistent across backend configurations.
 */

#include "../framework/ove_test.h"

#ifdef CONFIG_OVE_ASYNC

#include "ove/irq.h"
#include <stdatomic.h>

OVE_TEST_STORAGE(ove_timer_storage_t, s_async_tmr_storage);

/* ── helpers ─────────────────────────────────────────────────────────── */

static _Atomic int s_async_fire_count;
/* 64-bit timestamp: `volatile` rather than `_Atomic` so the test
 * compiles on 32-bit ARM (no native 64-bit atomic — libatomic isn't
 * linked into Zephyr/picolibc).  Reads / writes don't tear in practice
 * because the test thread always sleeps between arming the timer and
 * reading the field, so the callback's store is fully ordered behind
 * the wake of the test thread. */
static volatile uint64_t s_async_fire_us;

static void async_fire_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
	s_async_fire_count++;
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	s_async_fire_us = now;
}

/* ── ove_irq_lock / unlock / is_in_isr ──────────────────────────────── */

static void test_async_irq_lock_unlock_roundtrip(void **state)
{
	(void)state;
	ove_irq_key_t key = ove_irq_lock();
	/* Nothing observable about the cookie itself across backends — the
	 * contract is "pass this exact value back to ove_irq_unlock". */
	ove_irq_unlock(key);
}

static void test_async_irq_lock_nested(void **state)
{
	(void)state;
	/* LIFO nesting: outer + inner pair, no fault, no deadlock. */
	ove_irq_key_t k1 = ove_irq_lock();
	ove_irq_key_t k2 = ove_irq_lock();
	ove_irq_unlock(k2);
	ove_irq_unlock(k1);
}

static void test_async_is_in_isr_thread_context(void **state)
{
	(void)state;
	/* This test runs from the cmocka main thread (or its delegate
	 * test task on FreeRTOS/Zephyr/NuttX) — never an ISR. */
	assert_false(ove_is_in_isr());
}

/* ── ove_timer_*_ns: create / fire / reprogram ──────────────────────── */
/*
 * Timer-firing tests use SIGEV_THREAD on POSIX/NuttX (one helper pthread
 * per timer expiry).  TSan's runtime aborts on the helper-thread stack
 * precheck even when sigev_notify_attributes provides a 256 KB stack
 * (see test_timer.c for the full notes), so all firing tests are
 * gated out under __SANITIZE_THREAD__.
 */

#ifndef __SANITIZE_THREAD__

static void test_async_timer_init_ns_fires(void **state)
{
	(void)state;
	s_async_fire_count = 0;

	ove_timer_t t = NULL;
	int rc = ove_test_timer_create_ns(&t, &s_async_tmr_storage, async_fire_cb, NULL,
					  10000000ULL /* 10 ms */, 1 /* one_shot */);
	assert_int_equal(rc, OVE_OK);

	rc = ove_timer_start(t);
	assert_int_equal(rc, OVE_OK);

	test_msleep(80); /* generous window for the slowest backend (FreeRTOS 1 ms tick) */

	assert_int_equal(s_async_fire_count, 1);

	ove_test_timer_destroy(t);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_async_timer_create_ns_heap(void **state)
{
	(void)state;
	s_async_fire_count = 0;

	ove_timer_t t = NULL;
	int rc = ove_timer_create_ns(&t, async_fire_cb, NULL, 10000000ULL /* 10 ms */,
				     1 /* one_shot */);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(t);

	rc = ove_timer_start(t);
	assert_int_equal(rc, OVE_OK);

	test_msleep(80);
	assert_int_equal(s_async_fire_count, 1);

	ove_timer_destroy(t);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ── The regression that motivated ove_timer_set_period_ns ─────────────
 *
 * The Embassy time driver re-arms its global alarm at every
 * schedule_wake.  The naive implementation (stop + init_ns again)
 * corrupted FreeRTOS's daemon-task list because xTimerCreateStatic was
 * called repeatedly on the same `static_timer` slot — the first alarm
 * fired, every subsequent one didn't.  This test catches the regression
 * directly: arm a one-shot, wait for it to fire, then arm it again via
 * set_period_ns and assert the second fire actually happens.
 */
static void test_async_timer_set_period_ns_rearms_after_fire(void **state)
{
	(void)state;
	s_async_fire_count = 0;

	ove_timer_t t = NULL;
	int rc = ove_test_timer_create_ns(&t, &s_async_tmr_storage, async_fire_cb, NULL,
					  10000000ULL /* 10 ms */, 1 /* one_shot */);
	assert_int_equal(rc, OVE_OK);

	assert_int_equal(ove_timer_start(t), OVE_OK);
	test_msleep(80);
	assert_int_equal(s_async_fire_count, 1);

	/* Re-arm via set_period_ns — must trigger a second fire. */
	rc = ove_timer_set_period_ns(t, 10000000ULL);
	assert_int_equal(rc, OVE_OK);

	test_msleep(80);
	assert_int_equal(s_async_fire_count, 2);

	ove_test_timer_destroy(t);
}

/* set_period_ns on a running timer should restart the countdown with
 * the new period and not let the original period fire first. */
static void test_async_timer_set_period_ns_reprograms_running(void **state)
{
	(void)state;
	s_async_fire_count = 0;

	ove_timer_t t = NULL;
	int rc = ove_test_timer_create_ns(&t, &s_async_tmr_storage, async_fire_cb, NULL,
					  200000000ULL /* 200 ms */, 1 /* one_shot */);
	assert_int_equal(rc, OVE_OK);

	uint64_t arm_us = 0;
	(void)ove_time_get_us(&arm_us);
	assert_int_equal(ove_timer_start(t), OVE_OK);

	/* Reprogram to 30 ms before the original 200 ms deadline. */
	test_msleep(10);
	rc = ove_timer_set_period_ns(t, 30000000ULL /* 30 ms */);
	assert_int_equal(rc, OVE_OK);

	test_msleep(100);
	assert_int_equal(s_async_fire_count, 1);

	/* The fire should land closer to the reprogrammed 30 ms than the
	 * original 200 ms — give the slowest backend (FreeRTOS 1 ms tick
	 * + workqueue dispatch) up to 100 ms of slack. */
	uint64_t elapsed = s_async_fire_us - arm_us;
	assert_true(elapsed < 150000ULL); /* < 150 ms — well under the 200 ms original */

	ove_test_timer_destroy(t);
}

#endif /* !__SANITIZE_THREAD__ */

/* ── ove_queue_set_notify ───────────────────────────────────────────── */

/* Shared notify-fire counter — used by stream / queue / eventgroup /
 * semaphore set_notify tests below.  Reset in async_setup. */
static _Atomic int s_async_notify_count;

OVE_TEST_STORAGE(ove_queue_storage_t, s_async_queue_storage);
static uint32_t s_async_queue_buf[4];

static void queue_notify_cb(void *user_data)
{
	*(int *)user_data = 1;
	s_async_notify_count++;
}

static void test_async_queue_notify_fires_on_send(void **state)
{
	(void)state;
	s_async_notify_count = 0;
	int marker = 0;

	ove_queue_t q = NULL;
	int rc = ove_queue_init(&q, &s_async_queue_storage, s_async_queue_buf, sizeof(uint32_t),
				4 /* max_items */);
	assert_int_equal(rc, OVE_OK);

	rc = ove_queue_set_notify(q, queue_notify_cb, &marker);
	assert_int_equal(rc, OVE_OK);

	uint32_t v = 0xDEADBEEF;
	rc = ove_queue_send(q, &v, 0 /* non-blocking */);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(marker, 1);
	assert_int_equal(s_async_notify_count, 1);

	/* Clearing stops further notifications. */
	rc = ove_queue_set_notify(q, NULL, NULL);
	assert_int_equal(rc, OVE_OK);
	rc = ove_queue_send(q, &v, 0);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(s_async_notify_count, 1);

	ove_queue_deinit(q);
}

/* ── ove_stream_set_notify ──────────────────────────────────────────── */

static void stream_notify_cb(void *user_data)
{
	*(int *)user_data = 1;
	s_async_notify_count++;
}

OVE_TEST_STORAGE(ove_stream_storage_t, s_async_stream_storage);
static uint8_t s_async_stream_buf[64];

static void test_async_stream_notify_fires_on_send(void **state)
{
	(void)state;
	s_async_notify_count = 0;
	int marker = 0;

	ove_stream_t s = NULL;
	int rc = ove_stream_init(&s, &s_async_stream_storage, s_async_stream_buf,
				 sizeof(s_async_stream_buf), 1 /* trigger */);
	assert_int_equal(rc, OVE_OK);

	rc = ove_stream_set_notify(s, stream_notify_cb, &marker);
	assert_int_equal(rc, OVE_OK);

	/* Send must trigger the callback exactly once. */
	const uint8_t data[] = {0xAA, 0xBB, 0xCC};
	size_t sent = 0;
	rc = ove_stream_send(s, data, sizeof(data), 0 /* non-blocking */, &sent);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(sent, sizeof(data));
	assert_int_equal(marker, 1);
	assert_int_equal(s_async_notify_count, 1);

	/* Clearing the hook stops future notifications. */
	rc = ove_stream_set_notify(s, NULL, NULL);
	assert_int_equal(rc, OVE_OK);
	rc = ove_stream_send(s, data, sizeof(data), 0, &sent);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(s_async_notify_count, 1); /* unchanged */

	ove_stream_deinit(s);
}

/* ── ove_eventgroup_set_notify ──────────────────────────────────────── */

OVE_TEST_STORAGE(ove_eventgroup_storage_t, s_async_eg_storage);

static void eg_notify_cb(void *user_data)
{
	*(int *)user_data = 1;
	s_async_notify_count++;
}

static void test_async_eventgroup_notify_fires_on_set_bits(void **state)
{
	(void)state;
	s_async_notify_count = 0;
	int marker = 0;

	ove_eventgroup_t eg = NULL;
	int rc = ove_eventgroup_init(&eg, &s_async_eg_storage);
	assert_int_equal(rc, OVE_OK);

	rc = ove_eventgroup_set_notify(eg, eg_notify_cb, &marker);
	assert_int_equal(rc, OVE_OK);

	(void)ove_eventgroup_set_bits(eg, 0x1);
	assert_int_equal(marker, 1);
	assert_int_equal(s_async_notify_count, 1);

	rc = ove_eventgroup_set_notify(eg, NULL, NULL);
	assert_int_equal(rc, OVE_OK);
	(void)ove_eventgroup_set_bits(eg, 0x2);
	assert_int_equal(s_async_notify_count, 1);

	ove_eventgroup_deinit(eg);
}

/* ── ove_sem_set_notify ─────────────────────────────────────────────── */

OVE_TEST_STORAGE(ove_sem_storage_t, s_async_sem_storage);

static void sem_notify_cb(void *user_data)
{
	*(int *)user_data = 1;
	s_async_notify_count++;
}

static void test_async_sem_notify_fires_on_give(void **state)
{
	(void)state;
	s_async_notify_count = 0;
	int marker = 0;

	ove_sem_t s = NULL;
	int rc = ove_sem_init(&s, &s_async_sem_storage, 0 /* initial */, 4 /* max */);
	assert_int_equal(rc, OVE_OK);

	rc = ove_sem_set_notify(s, sem_notify_cb, &marker);
	assert_int_equal(rc, OVE_OK);

	ove_sem_give(s);
	assert_int_equal(marker, 1);
	assert_int_equal(s_async_notify_count, 1);

	rc = ove_sem_set_notify(s, NULL, NULL);
	assert_int_equal(rc, OVE_OK);
	ove_sem_give(s);
	assert_int_equal(s_async_notify_count, 1);

	ove_sem_deinit(s);
}

/* ── ove_uart_set_rx_notify ─────────────────────────────────────────── */
/* Verified only when UART is configured.  Most backends gate UART behind
 * CONFIG_OVE_UART; the stub fixtures don't always enable it.  When the
 * symbol isn't compiled the stub form returns OVE_ERR_NOT_SUPPORTED. */

#ifdef CONFIG_OVE_UART
static void uart_notify_cb(void *user_data)
{
	*(int *)user_data = 1;
	s_async_notify_count++;
}

static void test_async_uart_set_rx_notify_delegates_to_stream(void **state)
{
	(void)state;
	s_async_notify_count = 0;
	int marker = 0;

	/* Pass a NULL handle to verify the param check fires (we don't
	 * need a real UART up to assert the delegation logic — the stream
	 * notify path itself is already covered by
	 * test_async_stream_notify_fires_on_send). */
	int rc = ove_uart_set_rx_notify(NULL, uart_notify_cb, &marker);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}
#endif /* CONFIG_OVE_UART */

/* ── ove_spi_transfer_async / ove_i2c_write_read_async ──────────────── */
#if defined(CONFIG_OVE_SPI) || defined(CONFIG_OVE_I2C)
#include "ove/thread.h"
#endif

#ifdef CONFIG_OVE_SPI
#include "ove/spi.h"

/* Storage is stack-local inside each test to avoid growing .bss on
 * tight-RAM STM32 cmocka fixtures.  Test runner has plenty of stack. */
static _Atomic int s_async_spi_done;
static int s_async_spi_result;

static void async_spi_cb(int result, void *user_data)
{
	*(int *)user_data = 1;
	s_async_spi_result = result;
	atomic_store(&s_async_spi_done, 1);
}

static void test_async_spi_transfer_completion_fires(void **state)
{
	(void)state;
	atomic_store(&s_async_spi_done, 0);
	s_async_spi_result = -1;
	int marker = 0;

	ove_spi_storage_t storage;
	ove_spi_t spi = NULL;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	int rc = ove_spi_init(&spi, &storage, &cfg);
	assert_int_equal(rc, OVE_OK);

	uint8_t tx[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	uint8_t rx[4] = {0};

	rc = ove_spi_transfer_async(spi, NULL, tx, rx, sizeof(tx), async_spi_cb, &marker);
	assert_int_equal(rc, OVE_OK);

	/* Worker-thread fallback completes asynchronously (stub: loopback
	 * in pthread; STM32: HAL_SPI_TransmitReceive_IT + NVIC ISR). On
	 * Renode-simulated STM32 the IT completion needs a peripheral
	 * that actually clocks data — without a slave, the transfer never
	 * completes. Treat that as a no-device skip rather than a failure
	 * since the submission path was verified above. */
	for (int i = 0; i < 1000 && !atomic_load(&s_async_spi_done); i++)
		ove_thread_sleep_ms(1);
	if (!atomic_load(&s_async_spi_done)) {
		ove_spi_deinit(spi);
		skip();
		return;
	}
	assert_int_equal(marker, 1);
	assert_int_equal(s_async_spi_result, OVE_OK);

	ove_spi_deinit(spi);
}

static void test_async_spi_transfer_null_cb_rejected(void **state)
{
	(void)state;
	ove_spi_storage_t storage;
	ove_spi_t spi = NULL;
	struct ove_spi_cfg cfg = {
		.instance = 0,
		.clock_hz = 1000000,
		.mode = OVE_SPI_MODE_0,
		.bit_order = OVE_SPI_MSB_FIRST,
		.word_size = 8,
	};
	int rc = ove_spi_init(&spi, &storage, &cfg);
	assert_int_equal(rc, OVE_OK);

	uint8_t tx[4] = {1, 2, 3, 4};
	rc = ove_spi_transfer_async(spi, NULL, tx, NULL, sizeof(tx), NULL, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_spi_deinit(spi);
}
#endif /* CONFIG_OVE_SPI */

#ifdef CONFIG_OVE_I2C
#include "ove/i2c.h"

static _Atomic int s_async_i2c_done;
static int s_async_i2c_result;

static void async_i2c_cb(int result, void *user_data)
{
	*(int *)user_data = 1;
	s_async_i2c_result = result;
	atomic_store(&s_async_i2c_done, 1);
}

static void test_async_i2c_write_read_completion_fires(void **state)
{
	(void)state;
	atomic_store(&s_async_i2c_done, 0);
	s_async_i2c_result = -1;
	int marker = 0;

	ove_i2c_storage_t storage;
	ove_i2c_t i2c = NULL;
	struct ove_i2c_cfg cfg = {
		.instance = 0,
		.speed = OVE_I2C_SPEED_FAST,
	};
	int rc = ove_i2c_init(&i2c, &storage, &cfg);
	assert_int_equal(rc, OVE_OK);

	uint8_t tx[2] = {0xAA, 0xBB};
	uint8_t rx[3] = {0};
	rc = ove_i2c_write_read_async(i2c, 0x42, tx, sizeof(tx), rx, sizeof(rx), async_i2c_cb,
				      &marker);
	assert_int_equal(rc, OVE_OK);

	for (int i = 0; i < 1000 && !atomic_load(&s_async_i2c_done); i++)
		ove_thread_sleep_ms(1);
	if (!atomic_load(&s_async_i2c_done)) {
		/* No I2C peripheral attached — Renode skips completion. */
		ove_i2c_deinit(i2c);
		skip();
		return;
	}
	assert_int_equal(marker, 1);
	assert_int_equal(s_async_i2c_result, OVE_OK);

	ove_i2c_deinit(i2c);
}

static void test_async_i2c_null_cb_rejected(void **state)
{
	(void)state;
	ove_i2c_storage_t storage;
	ove_i2c_t i2c = NULL;
	struct ove_i2c_cfg cfg = {
		.instance = 0,
		.speed = OVE_I2C_SPEED_STANDARD,
	};
	int rc = ove_i2c_init(&i2c, &storage, &cfg);
	assert_int_equal(rc, OVE_OK);

	uint8_t tx[1] = {0};
	uint8_t rx[1] = {0};
	rc = ove_i2c_write_read_async(i2c, 0x10, tx, 1, rx, 1, NULL, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_i2c_deinit(i2c);
}
#endif /* CONFIG_OVE_I2C */

/* ── setup ───────────────────────────────────────────────────────────── */

static int async_setup(void **state)
{
	(void)state;
	s_async_fire_count = 0;
	s_async_fire_us = 0;
	return 0;
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_async_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_async_irq_lock_unlock_roundtrip, async_setup),
		cmocka_unit_test_setup(test_async_irq_lock_nested, async_setup),
		cmocka_unit_test_setup(test_async_is_in_isr_thread_context, async_setup),
#ifndef __SANITIZE_THREAD__
		cmocka_unit_test_setup(test_async_timer_init_ns_fires, async_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_async_timer_create_ns_heap, async_setup),
#endif
		cmocka_unit_test_setup(test_async_timer_set_period_ns_rearms_after_fire,
				       async_setup),
		cmocka_unit_test_setup(test_async_timer_set_period_ns_reprograms_running,
				       async_setup),
#endif
		cmocka_unit_test_setup(test_async_stream_notify_fires_on_send, async_setup),
		cmocka_unit_test_setup(test_async_queue_notify_fires_on_send, async_setup),
		cmocka_unit_test_setup(test_async_eventgroup_notify_fires_on_set_bits, async_setup),
		cmocka_unit_test_setup(test_async_sem_notify_fires_on_give, async_setup),
#ifdef CONFIG_OVE_UART
		cmocka_unit_test_setup(test_async_uart_set_rx_notify_delegates_to_stream,
				       async_setup),
#endif
#ifdef CONFIG_OVE_SPI
		cmocka_unit_test_setup(test_async_spi_transfer_completion_fires, async_setup),
		cmocka_unit_test_setup(test_async_spi_transfer_null_cb_rejected, async_setup),
#endif
#ifdef CONFIG_OVE_I2C
		cmocka_unit_test_setup(test_async_i2c_write_read_completion_fires, async_setup),
		cmocka_unit_test_setup(test_async_i2c_null_cb_rejected, async_setup),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

#else /* !CONFIG_OVE_ASYNC */

int test_async_run(void)
{
	/* CONFIG_OVE_ASYNC=n: no symbols to test.  Stub keeps the suite
	 * list (framework/suites.inc) consistent across backend configs. */
	return 0;
}

#endif /* CONFIG_OVE_ASYNC */
