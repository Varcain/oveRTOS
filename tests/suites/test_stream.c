/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "../framework/ove_test_isr.h"
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
	OVE_TEST_ASSERT_OK(ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1));
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
	OVE_TEST_ASSERT_OK(ove_stream_send(s, data, 64, 0, &sent));
	assert_int_equal(sent, 64);

	uint8_t out[64] = {0};
	size_t received = 0;
	OVE_TEST_ASSERT_OK(ove_stream_receive(s, out, 64, 0, &received));
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
	OVE_TEST_ASSERT_OK(ove_stream_send(s, tx, 6, 0, NULL));

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
	OVE_TEST_ASSERT_OK(ove_stream_send(s, tx, 4, 0, NULL));

	OVE_TEST_ASSERT_OK(ove_stream_reset(s));

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
	OVE_TEST_ASSERT_OK(ove_stream_send(s, tx, 10, 0, NULL));

	uint8_t rx[5] = {0};
	size_t received = 0;
	OVE_TEST_ASSERT_OK(ove_stream_receive(s, rx, 5, 0, &received));
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
	OVE_TEST_ASSERT_OK(ove_stream_send(s, data, 8, 0, NULL));

	/* Buffer is full; send should transfer 0 bytes */
	uint8_t extra = 0x22;
	size_t sent2 = 0;
	OVE_TEST_ASSERT_OK(ove_stream_send(s, &extra, 1, OVE_MS(10), &sent2));
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
	OVE_TEST_ASSERT_OK(ove_stream_receive(s, rx, sizeof(rx), OVE_MS(10), &received));
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
	/* Worker thread: cmocka asserts belong on the runner, which checks the bytes. */
	int rc = ove_stream_send(s, a, 2, OVE_WAIT_FOREVER, NULL);
	test_msleep(50);
	rc = ove_stream_send(s, b, 2, OVE_WAIT_FOREVER, NULL);
	(void)rc;
}

static void test_stream_trigger(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	/* trigger = 4: a blocked receive must wait until >= 4 bytes are present
	 * (ove/stream.h contract). A backend that ignores trigger (wake on any
	 * byte) would return after the producer's first 2 bytes -> received==2. */
	OVE_TEST_ASSERT_OK(ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 4));

	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_th_storage, "trig_prod", trigger_producer_fn,
					       s, s_th_stack, 4096));

	uint8_t rx[16] = {0};
	size_t received = 0;
	int rc = ove_stream_receive(s, rx, sizeof(rx), OVE_MS(2000), &received);
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
	OVE_TEST_ASSERT_OK(ove_stream_receive(s, rx, sizeof(rx), 0, &received));
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
	OVE_TEST_ASSERT_OK(ove_stream_send(s, tx, sizeof(tx), 0, NULL));

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
		/* Worker thread: the runner checks what arrives, not this rc. */
		int rc = ove_stream_send(pa->stream, &byte, 1, OVE_MS(100), &sent);
		(void)rc;
	}
}

static void test_stream_cross_thread(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	ove_test_stream_create(&s, &s_strm_storage_small, s_strm_buf_small, 65, 1);

	struct stream_producer_arg pa = {.stream = s, .count = 100};

	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_th_storage, "producer", stream_producer_fn,
					       &pa, s_th_stack, 4096));

	/* Consumer: receive all bytes */
	int received_total = 0;
	for (int tries = 0; tries < 200 && received_total < 100; tries++) {
		uint8_t buf[32];
		size_t received = 0;
		OVE_TEST_ASSERT_OK(ove_stream_receive(s, buf, sizeof(buf), OVE_MS(20), &received));
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

/* ── timed waits and interrupt context ───────────────────────────────── */

static void drip_producer_fn(void *arg)
{
	ove_stream_t s = (ove_stream_t)arg;
	for (uint8_t i = 0; i < 4; i++) {
		test_msleep(30);
		/* Worker thread: the runner checks the receive, not this rc. */
		int rc = ove_stream_send(s, &i, 1, OVE_WAIT_FOREVER, NULL);
		(void)rc;
	}
}

/* A timed receive keeps its deadline across wakeups that bring too few
 * bytes for the trigger: NuttX restarted the timeout on every one, so a
 * 50 ms receive fed a byte every 30 ms returned 4 bytes after ~125 ms. */
static void test_stream_receive_deadline_spans_wakeups(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	OVE_TEST_ASSERT_OK(ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 4));
	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_th_storage, "drip", drip_producer_fn, s,
					       s_th_stack, 4096));

	uint64_t t0 = 0;
	uint64_t t1 = 0;
	(void)ove_time_get_us(&t0);
	uint8_t rx[16];
	size_t received = 99;
	OVE_TEST_ASSERT_OK(ove_stream_receive(s, rx, sizeof(rx), OVE_MS(50), &received));
	(void)ove_time_get_us(&t1);
	assert_int_equal(received, 0);
	assert_true(t1 - t0 < 100000);

	test_msleep(150); /* the producer finishes */
	ove_test_thread_destroy(th);
	ove_test_stream_destroy(s);
}

/* A timeout longer than the tick counter can express still waits: on NuttX
 * a 30-day receive at a 1 kHz tick returned at once with nothing. */
static void test_stream_receive_long_timeout_waits(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	OVE_TEST_ASSERT_OK(ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 4));
	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_th_storage, "drip", drip_producer_fn, s,
					       s_th_stack, 4096));

	uint8_t rx[16];
	size_t received = 0;
	OVE_TEST_ASSERT_OK(
		ove_stream_receive(s, rx, sizeof(rx), OVE_MS(30ULL * 24 * 3600 * 1000), &received));
	assert_int_equal(received, 4);

	test_msleep(20); /* the producer returns from its last send */
	ove_test_thread_destroy(th);
	ove_test_stream_destroy(s);
}

struct isr_stream_call {
	ove_stream_t s;
	const uint8_t *data;
	size_t len;
	uint8_t *buf;
	size_t buf_len;
	size_t sent;
	size_t received;
	int rc_send;
	int rc_recv;
	bool in_isr;
	int done;
};

static void isr_send(void *arg)
{
	struct isr_stream_call *c = arg;
	c->rc_send = ove_stream_send_from_isr(c->s, c->data, c->len, &c->sent);
}

static void isr_send_then_receive(void *arg)
{
	struct isr_stream_call *c = arg;
	c->rc_send = ove_stream_send_from_isr(c->s, c->data, c->len, &c->sent);
	c->rc_recv = ove_stream_receive_from_isr(c->s, c->buf, c->buf_len, &c->received);
}

static void isr_sender_thread_fn(void *arg)
{
	struct isr_stream_call *c = arg;
	test_msleep(20); /* the runner is waiting in receive by now */
	c->in_isr = ove_test_run_in_isr(isr_send, c);
	__atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
}

/* An interrupt that brings the stream to its trigger level wakes a thread
 * waiting in receive. */
static void test_stream_isr_send_wakes_receiver(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	OVE_TEST_ASSERT_OK(ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 4));
	static const uint8_t tx[4] = {0xB1, 0xB2, 0xB3, 0xB4};
	struct isr_stream_call c = {.s = s, .data = tx, .len = sizeof(tx), .rc_send = -1};
	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_th_storage, "isr_tx", isr_sender_thread_fn,
					       &c, s_th_stack, 4096));

	uint8_t rx[8] = {0};
	size_t received = 0;
	OVE_TEST_ASSERT_OK(ove_stream_receive(s, rx, sizeof(rx), OVE_MS(1000), &received));
	assert_int_equal(received, 4);
	assert_memory_equal(rx, tx, 4);
	for (int i = 0; i < 1000 && !__atomic_load_n(&c.done, __ATOMIC_ACQUIRE); i++)
		test_msleep(1);
	assert_true(__atomic_load_n(&c.done, __ATOMIC_ACQUIRE));
	assert_int_equal(c.rc_send, OVE_OK);
	assert_int_equal(c.sent, 4);
#ifndef CONFIG_OVE_RTOS_POSIX
	assert_true(c.in_isr);
#endif
	ove_test_thread_destroy(th);
	ove_test_stream_destroy(s);
}

/* From an interrupt a send into a nearly full stream takes what fits, and
 * a receive takes what is there. */
static void test_stream_isr_partial_send_and_drain(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	OVE_TEST_ASSERT_OK(ove_test_stream_create(&s, &s_strm_storage_tiny, s_strm_buf_tiny, 8, 1));
	static const uint8_t six[6] = {1, 2, 3, 4, 5, 6};
	static const uint8_t four[4] = {7, 8, 9, 10};
	OVE_TEST_ASSERT_OK(ove_stream_send(s, six, sizeof(six), 0, NULL));

	uint8_t rx[16] = {0};
	struct isr_stream_call c = {.s = s,
				    .data = four,
				    .len = sizeof(four),
				    .buf = rx,
				    .buf_len = sizeof(rx),
				    .rc_send = -1,
				    .rc_recv = -1};
	bool in_isr = ove_test_run_in_isr(isr_send_then_receive, &c);
#ifndef CONFIG_OVE_RTOS_POSIX
	assert_true(in_isr);
#else
	(void)in_isr;
#endif
	assert_int_equal(c.rc_send, OVE_OK);
	assert_int_equal(c.sent, 2);
	assert_int_equal(c.rc_recv, OVE_OK);
	assert_int_equal(c.received, 8);
	static const uint8_t expect[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	assert_memory_equal(rx, expect, 8);
	ove_test_stream_destroy(s);
}

/* Interrupts writing while a thread streams through the same ring: on
 * NuttX the interrupt took the thread's lock, wrote the ring under it and
 * released it, which corrupted the ring and the lock within a few writes.
 * The thread runs below the runner and never sleeps, so each interrupt
 * (all of them come from the tick) lands somewhere in its loop.  It does
 * not catch a thread path that merely lost its critical section, though:
 * on QEMU, removing them from send and receive went unnoticed. */
struct stream_hammer {
	ove_stream_t s;
	volatile int stop;
	volatile int done;
	int seq_errors;
	int markers;
};

static void stream_hammer_fn(void *arg)
{
	struct stream_hammer *h = arg;
	uint8_t next_tx = 0;
	uint8_t next_rx = 0;
	for (;;) {
		int stopping = __atomic_load_n(&h->stop, __ATOMIC_ACQUIRE);
		uint8_t chunk[32];
		if (!stopping) {
			for (size_t i = 0; i < sizeof(chunk); i++)
				chunk[i] = (uint8_t)((next_tx + i) & 0x7F);
			size_t sent = 0;
			int rc = ove_stream_send(h->s, chunk, sizeof(chunk), 0, &sent);
			(void)rc;
			next_tx = (uint8_t)((next_tx + sent) & 0x7F);
		}
		uint8_t rx[64];
		size_t got = 0;
		int rc = ove_stream_receive(h->s, rx, sizeof(rx), 0, &got);
		(void)rc;
		for (size_t i = 0; i < got; i++) {
			if (rx[i] & 0x80) {
				h->markers++;
			} else {
				if (rx[i] != next_rx)
					h->seq_errors++;
				next_rx = (uint8_t)((rx[i] + 1) & 0x7F);
			}
		}
		if (stopping && got == 0)
			break;
#ifdef CONFIG_OVE_RTOS_POSIX
		ove_thread_yield(); /* the host has no priorities */
#endif
	}
	__atomic_store_n(&h->done, 1, __ATOMIC_RELEASE);
}

struct isr_marker {
	ove_stream_t s;
	uint8_t byte;
	size_t sent;
};

static void isr_send_marker(void *arg)
{
	struct isr_marker *m = arg;
	int rc = ove_stream_send_from_isr(m->s, &m->byte, 1, &m->sent);
	(void)rc;
}

static void test_stream_isr_send_while_thread_streams(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	OVE_TEST_ASSERT_OK(
		ove_test_stream_create(&s, &s_strm_storage_small, s_strm_buf_small, 65, 1));
	struct stream_hammer h = {.s = s};
	ove_thread_t th = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_thread_init(&th, &s_th_storage, "hammer", stream_hammer_fn, &h,
					   OVE_PRIO_IDLE, 4096, s_th_stack));
#else
	OVE_TEST_ASSERT_OK(
		ove_thread_create(&th, "hammer", stream_hammer_fn, &h, OVE_PRIO_IDLE, 4096));
#endif

	int markers_sent = 0;
	int not_in_isr = 0;
	for (int k = 0; k < 200; k++) {
		struct isr_marker m = {.s = s, .byte = (uint8_t)(0x80 | (k & 0x7F))};
		if (!ove_test_run_in_isr(isr_send_marker, &m))
			not_in_isr++;
		markers_sent += (int)m.sent;
	}
	__atomic_store_n(&h.stop, 1, __ATOMIC_RELEASE);
	for (int i = 0; i < 2000 && !__atomic_load_n(&h.done, __ATOMIC_ACQUIRE); i++)
		test_msleep(1);

	assert_true(__atomic_load_n(&h.done, __ATOMIC_ACQUIRE));
#ifndef CONFIG_OVE_RTOS_POSIX
	assert_int_equal(not_in_isr, 0);
#else
	(void)not_in_isr;
#endif
	assert_int_equal(h.seq_errors, 0);
	assert_int_equal(h.markers, markers_sent);
	assert_int_equal(ove_stream_bytes_available(s), 0);
	ove_test_thread_destroy(th);
	ove_test_stream_destroy(s);
}

/* NULL arguments from an interrupt are refused, not dereferenced. */
static void isr_null_args(void *arg)
{
	struct isr_stream_call *c = arg;
	uint8_t b[4] = {0};
	c->rc_send = ove_stream_send_from_isr(c->s, NULL, 4, NULL);
	c->rc_recv = ove_stream_receive_from_isr(c->s, NULL, 4, NULL);
	c->sent = (size_t)ove_stream_send_from_isr(NULL, b, sizeof(b), NULL);
	c->received = (size_t)ove_stream_receive_from_isr(NULL, b, sizeof(b), NULL);
}

static void test_stream_isr_null_args(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	OVE_TEST_ASSERT_OK(ove_test_stream_create(&s, &s_strm_storage_tiny, s_strm_buf_tiny, 8, 1));
	struct isr_stream_call c = {.s = s, .rc_send = 0, .rc_recv = 0};
	bool in_isr = ove_test_run_in_isr(isr_null_args, &c);
#ifndef CONFIG_OVE_RTOS_POSIX
	assert_true(in_isr);
#else
	(void)in_isr;
#endif
	assert_int_equal(c.rc_send, OVE_ERR_INVALID_PARAM);
	assert_int_equal(c.rc_recv, OVE_ERR_INVALID_PARAM);
	assert_int_equal((int)c.sent, OVE_ERR_INVALID_PARAM);
	assert_int_equal((int)c.received, OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_stream_bytes_available(s), 0);
	ove_test_stream_destroy(s);
}

static void test_stream_space_available(void **state)
{
	(void)state;
	ove_stream_t s = NULL;
	OVE_TEST_ASSERT_OK(ove_test_stream_create(&s, &s_strm_storage, s_strm_buf, 256, 1));
	assert_int_equal(ove_stream_space_available(s), 256);

	uint8_t d[40] = {0};
	size_t n = 0;
	OVE_TEST_ASSERT_OK(ove_stream_send(s, d, sizeof(d), 0, &n));
	assert_int_equal(n, sizeof(d));
	assert_int_equal(ove_stream_space_available(s), 256 - sizeof(d));

	OVE_TEST_ASSERT_OK(ove_stream_receive(s, d, 15, 0, &n));
	assert_int_equal(n, 15);
	assert_int_equal(ove_stream_bytes_available(s), sizeof(d) - 15);
	assert_int_equal(ove_stream_space_available(s), 256 - (sizeof(d) - 15));
	ove_test_stream_destroy(s);

	assert_int_equal(ove_stream_space_available(NULL), 0);
	assert_int_equal(ove_stream_bytes_available(NULL), 0);
}

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
		cmocka_unit_test(test_stream_space_available),
		cmocka_unit_test(test_stream_receive_deadline_spans_wakeups),
		cmocka_unit_test(test_stream_receive_long_timeout_waits),
		cmocka_unit_test(test_stream_isr_send_wakes_receiver),
		cmocka_unit_test(test_stream_isr_partial_send_and_drain),
		cmocka_unit_test(test_stream_isr_send_while_thread_streams),
		cmocka_unit_test(test_stream_isr_null_args),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
