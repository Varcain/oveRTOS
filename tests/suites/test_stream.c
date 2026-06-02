/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <string.h>
#include <stdatomic.h>

OVE_TEST_STORAGE(ove_stream_storage_t, s_strm_storage);
static uint8_t s_strm_buf[256 + 1];
OVE_TEST_STORAGE(ove_stream_storage_t, s_strm_storage_small);
static uint8_t s_strm_buf_small[65 + 1];
OVE_TEST_STORAGE(ove_stream_storage_t, s_strm_storage_tiny);
static uint8_t s_strm_buf_tiny[8 + 1];
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_stream_create_destroy(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	int rc = ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(s);
	ove_test_stream_destroy(s);
}

static void test_stream_send_receive(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1);

	const uint8_t tx[] = "hello";
	size_t sent = 0;
	int rc = ove_stream_send(s, tx, sizeof(tx), 0, &sent);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(sent, sizeof(tx));

	uint8_t rx[32] = {0};
	size_t received = 0;
	rc = ove_stream_receive(s, rx, sizeof(rx), 0, &received);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(received, sizeof(tx));
	assert_string_equal((char *)rx, "hello");

	ove_test_stream_destroy(s);
}

static void test_stream_fill_drain(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage_small, s_strm_buf_small, 65, 1);

	uint8_t data[64];
	memset(data, 0xAB, sizeof(data));

	size_t sent = 0;
	ove_stream_send(s, data, 64, 0, &sent);
	assert_int_equal(sent, 64);

	uint8_t out[64] = {0};
	size_t received = 0;
	ove_stream_receive(s, out, 64, 0, &received);
	assert_int_equal(received, 64);
	assert_memory_equal(out, data, 64);

	ove_test_stream_destroy(s);
}

static void test_stream_bytes_available(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1);

	size_t avail = ove_stream_bytes_available(s);
	assert_int_equal(avail, 0);

	const uint8_t tx[] = "abcdef";
	ove_stream_send(s, tx, 6, 0, NULL);

	avail = ove_stream_bytes_available(s);
	assert_int_equal(avail, 6);

	ove_test_stream_destroy(s);
}

static void test_stream_reset(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1);

	const uint8_t tx[] = "data";
	ove_stream_send(s, tx, 4, 0, NULL);

	int rc = ove_stream_reset(s);
	assert_int_equal(rc, OVE_OK);

	size_t avail = ove_stream_bytes_available(s);
	assert_int_equal(avail, 0);

	ove_test_stream_destroy(s);
}

static void test_stream_partial_receive(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1);

	const uint8_t tx[] = "abcdefghij"; /* 10 bytes */
	ove_stream_send(s, tx, 10, 0, NULL);

	uint8_t rx[5] = {0};
	size_t received = 0;
	ove_stream_receive(s, rx, 5, 0, &received);
	assert_int_equal(received, 5);
	assert_memory_equal(rx, "abcde", 5);

	/* 5 bytes should remain */
	size_t avail = ove_stream_bytes_available(s);
	assert_int_equal(avail, 5);

	ove_test_stream_destroy(s);
}

static void test_stream_send_timeout_full(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage_tiny, s_strm_buf_tiny, 8, 1);

	uint8_t data[8];
	memset(data, 0x11, sizeof(data));
	ove_stream_send(s, data, 8, 0, NULL);

	/* Buffer is full; send should transfer 0 bytes */
	uint8_t extra = 0x22;
	size_t sent2 = 0;
	ove_stream_send(s, &extra, 1, OVE_MS(10), &sent2);
	assert_int_equal(sent2, 0);

	ove_test_stream_destroy(s);
}

static void test_stream_receive_timeout_empty(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1);

	uint8_t rx[16];
	size_t received = 0;
	ove_stream_receive(s, rx, sizeof(rx), OVE_MS(10), &received);
	assert_int_equal(received, 0);

	ove_test_stream_destroy(s);
}

static void trigger_producer_fn(void *arg)
{
	ove_stream_t s = (ove_stream_t)arg;
	const uint8_t a[] = {0xA1, 0xA2};
	const uint8_t b[] = {0xA3, 0xA4};
	/* Two bytes (below the trigger), a gap so a correct receiver is left
	 * blocked, then the two that complete the trigger threshold. */
	ove_stream_send(s, a, 2, OVE_WAIT_FOREVER, NULL);
	test_msleep(50);
	ove_stream_send(s, b, 2, OVE_WAIT_FOREVER, NULL);
}

static void test_stream_trigger(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	/* trigger = 4: a blocked receive must wait until >= 4 bytes are present
	 * (ove/stream.h contract). A backend that ignores trigger (wake on any
	 * byte) would return after the producer's first 2 bytes -> received==2. */
	int rc = ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 4);
	assert_int_equal(rc, OVE_OK);

	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "trig_prod", trigger_producer_fn, s, s_th_stack,
			    4096);

	uint8_t rx[16] = {0};
	size_t received = 0;
	rc = ove_stream_receive(s, rx, sizeof(rx), OVE_MS(2000), &received);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(received, 4);
	assert_int_equal(rx[0], 0xA1);
	assert_int_equal(rx[3], 0xA4);

	ove_test_thread_destroy(th);
	ove_test_stream_destroy(s);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_stream_create_null_handle(void **state)
{
	(void)state;
	int rc = ove_stream_create(NULL, 64, 1);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}
#endif

static void test_stream_send_from_isr(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1);

	const uint8_t tx[] = "isr_send";
	size_t sent = 0;
	int rc = ove_stream_send_from_isr(s, tx, sizeof(tx), &sent);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(sent, sizeof(tx));

	uint8_t rx[32] = {0};
	size_t received = 0;
	ove_stream_receive(s, rx, sizeof(rx), 0, &received);
	assert_int_equal(received, sizeof(tx));
	assert_string_equal((char *)rx, "isr_send");

	ove_test_stream_destroy(s);
}

static void test_stream_receive_from_isr(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1);

	const uint8_t tx[] = "isr_recv";
	ove_stream_send(s, tx, sizeof(tx), 0, NULL);

	uint8_t rx[32] = {0};
	size_t received = 0;
	int rc = ove_stream_receive_from_isr(s, rx, sizeof(rx), &received);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(received, sizeof(tx));
	assert_string_equal((char *)rx, "isr_recv");

	ove_test_stream_destroy(s);
}

/* ── cross-thread test ───────────────────────────────────────────────── */

struct stream_producer_arg {
	ove_stream_t stream;
	int count;
};

static void stream_producer_fn(void *arg)
{
	struct stream_producer_arg *pa = (struct stream_producer_arg *)arg;
	for (int i = 0; i < pa->count; i++) {
		uint8_t byte = (uint8_t)(i & 0xFF);
		size_t sent = 0;
		ove_stream_send(pa->stream, &byte, 1, OVE_MS(100), &sent);
	}
}

static void test_stream_cross_thread(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage_small, s_strm_buf_small, 65, 1);

	struct stream_producer_arg pa = {.stream = s, .count = 100};

	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "producer", stream_producer_fn, &pa, s_th_stack,
			    4096);

	/* Consumer: receive all bytes */
	int received_total = 0;
	for (int tries = 0; tries < 200 && received_total < 100; tries++) {
		uint8_t buf[32];
		size_t received = 0;
		ove_stream_receive(s, buf, sizeof(buf), OVE_MS(20), &received);
		for (size_t j = 0; j < received; j++) {
			assert_int_equal(buf[j], (uint8_t)(received_total & 0xFF));
			received_total++;
		}
	}

	assert_int_equal(received_total, 100);

	ove_test_thread_destroy(th);
	ove_test_stream_destroy(s);
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_stream_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_stream_create_destroy),
		cmocka_unit_test(test_stream_send_receive),
		cmocka_unit_test(test_stream_fill_drain),
		cmocka_unit_test(test_stream_bytes_available),
		cmocka_unit_test(test_stream_reset),
		cmocka_unit_test(test_stream_partial_receive),
		cmocka_unit_test(test_stream_send_timeout_full),
		cmocka_unit_test(test_stream_receive_timeout_empty),
		cmocka_unit_test(test_stream_trigger),
		cmocka_unit_test(test_stream_cross_thread),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_stream_create_null_handle),
#endif
		cmocka_unit_test(test_stream_send_from_isr),
		cmocka_unit_test(test_stream_receive_from_isr),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
