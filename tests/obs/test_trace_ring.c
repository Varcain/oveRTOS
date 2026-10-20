/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host test for the lock-free MPSC trace ring
 * (backends/common/ove_trace_ring.c).
 *
 * Standalone (no CMocka) so it also runs cleanly under ThreadSanitizer.
 * Built twice by tests/CMakeLists.txt: plain and -fsanitize=thread.
 */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ove_config.h"
#include "ove_trace_ring.h"

#define RING CONFIG_OVE_TRACE_RING_SIZE

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

static struct ove_trace_record make_rec(uint32_t tid, uint64_t seq)
{
	struct ove_trace_record r;
	memset(&r, 0, sizeof(r));
	r.ts_us = seq;
	r.tid = tid;
	r.kind = 1;
	r.code = (uint8_t)(seq & 0xFF);
	r.object = (uint32_t)seq;
	return r;
}

/* Drain until empty; returns number of records discarded. */
static size_t drain_all(void)
{
	static struct ove_trace_record buf[256];
	size_t total = 0, n;
	while ((n = ove_trace_ring_drain(buf, 256)) > 0)
		total += n;
	(void)ove_trace_ring_dropped_get();
	return total;
}

/* ── Single-threaded behaviour ───────────────────────────────────── */

static void test_fifo_order(void)
{
	drain_all();
	for (uint64_t i = 0; i < 100; i++) {
		struct ove_trace_record r = make_rec(7, i);
		CHECK(ove_trace_ring_push(&r));
	}
	struct ove_trace_record out[128];
	size_t n = ove_trace_ring_drain(out, 128);
	CHECK(n == 100);
	for (size_t i = 0; i < n; i++) {
		CHECK(out[i].tid == 7);
		CHECK(out[i].ts_us == i);
		CHECK(out[i].object == (uint32_t)i);
	}
	CHECK(ove_trace_ring_dropped_get() == 0);
	CHECK(ove_trace_ring_drain(out, 128) == 0);
}

static void test_partial_drain(void)
{
	drain_all();
	for (uint64_t i = 0; i < 10; i++) {
		struct ove_trace_record r = make_rec(1, i);
		CHECK(ove_trace_ring_push(&r));
	}
	struct ove_trace_record out[4];
	uint64_t expect = 0;
	size_t n;
	while ((n = ove_trace_ring_drain(out, 4)) > 0) {
		CHECK(n <= 4);
		for (size_t i = 0; i < n; i++)
			CHECK(out[i].ts_us == expect++);
	}
	CHECK(expect == 10);
}

static void test_overflow_counts_drops(void)
{
	drain_all();
	for (uint64_t i = 0; i < RING; i++) {
		struct ove_trace_record r = make_rec(2, i);
		CHECK(ove_trace_ring_push(&r));
	}
	for (int i = 0; i < 5; i++) {
		struct ove_trace_record r = make_rec(2, 0);
		CHECK(!ove_trace_ring_push(&r));
	}
	CHECK(ove_trace_ring_dropped_get() == 5);
	CHECK(ove_trace_ring_dropped_get() == 0); /* read-and-clear */
	CHECK(drain_all() == RING);
}

static void test_wraparound(void)
{
	/* Push/drain across many wraps so slot reuse and commit-flag reset
	 * are exercised (write/read indices pass RING several times). */
	drain_all();
	struct ove_trace_record out[64];
	uint64_t seq_in = 0, seq_out = 0;
	for (int round = 0; round < 5 * RING / 48; round++) {
		for (int i = 0; i < 48; i++) {
			struct ove_trace_record r = make_rec(3, seq_in++);
			CHECK(ove_trace_ring_push(&r));
		}
		size_t n;
		while ((n = ove_trace_ring_drain(out, 64)) > 0)
			for (size_t i = 0; i < n; i++)
				CHECK(out[i].ts_us == seq_out++);
	}
	CHECK(seq_in == seq_out);
	CHECK(ove_trace_ring_dropped_get() == 0);
}

/* ── Concurrency ─────────────────────────────────────────────────── */

#define PRODUCERS 4

struct prod_arg {
	uint32_t tid;
	uint64_t count;
	int retry_when_full;
	atomic_int *go;
};

static void *producer(void *p)
{
	struct prod_arg *a = (struct prod_arg *)p;
	while (!atomic_load(a->go))
		;
	for (uint64_t i = 0; i < a->count; i++) {
		struct ove_trace_record r = make_rec(a->tid, i);
		while (!ove_trace_ring_push(&r)) {
			if (!a->retry_when_full)
				break;
			sched_yield();
		}
	}
	return NULL;
}

/*
 * Regression for the old try-lock: concurrent producers into a ring with
 * room for everything must lose nothing. The try-lock version dropped on
 * every collision.
 */
static void test_concurrent_no_drops_when_room(void)
{
	drain_all();
	const uint64_t per = RING / PRODUCERS;
	atomic_int go = 0;
	pthread_t th[PRODUCERS];
	struct prod_arg args[PRODUCERS];
	for (int i = 0; i < PRODUCERS; i++) {
		args[i] = (struct prod_arg){
			.tid = 100u + (uint32_t)i, .count = per, .retry_when_full = 0, .go = &go};
		pthread_create(&th[i], NULL, producer, &args[i]);
	}
	atomic_store(&go, 1);
	for (int i = 0; i < PRODUCERS; i++)
		pthread_join(th[i], NULL);

	CHECK(ove_trace_ring_dropped_get() == 0);

	uint64_t next[PRODUCERS] = {0};
	struct ove_trace_record out[256];
	size_t n, total = 0;
	while ((n = ove_trace_ring_drain(out, 256)) > 0) {
		for (size_t i = 0; i < n; i++) {
			uint32_t p = out[i].tid - 100u;
			CHECK(p < PRODUCERS);
			if (p >= PRODUCERS)
				continue;
			CHECK(out[i].ts_us == next[p]); /* per-producer FIFO */
			next[p] = out[i].ts_us + 1;
		}
		total += n;
	}
	CHECK(total == per * PRODUCERS);
}

struct drain_arg {
	atomic_int *producers_done;
	uint64_t next[PRODUCERS];
	uint64_t total;
	int bad;
};

static void *drainer(void *p)
{
	struct drain_arg *d = (struct drain_arg *)p;
	struct ove_trace_record out[128];
	for (;;) {
		int done = atomic_load(d->producers_done);
		size_t n = ove_trace_ring_drain(out, 128);
		for (size_t i = 0; i < n; i++) {
			uint32_t pi = out[i].tid - 200u;
			if (pi >= PRODUCERS || out[i].ts_us != d->next[pi] ||
			    out[i].object != (uint32_t)out[i].ts_us) {
				d->bad++;
				continue;
			}
			d->next[pi]++;
		}
		d->total += n;
		if (n == 0 && done)
			break;
		if (n == 0)
			sched_yield();
	}
	return NULL;
}

/*
 * Producers race a concurrent drainer, retrying when the ring is full.
 * Every record must arrive exactly once, uncorrupted, in per-producer
 * order.
 */
static void test_concurrent_with_drainer(void)
{
	drain_all();
	const uint64_t per = 50000;
	atomic_int go = 0, done = 0;
	pthread_t th[PRODUCERS], dth;
	struct prod_arg args[PRODUCERS];
	struct drain_arg d;
	memset(&d, 0, sizeof(d));
	d.producers_done = &done;

	pthread_create(&dth, NULL, drainer, &d);
	for (int i = 0; i < PRODUCERS; i++) {
		args[i] = (struct prod_arg){
			.tid = 200u + (uint32_t)i, .count = per, .retry_when_full = 1, .go = &go};
		pthread_create(&th[i], NULL, producer, &args[i]);
	}
	atomic_store(&go, 1);
	for (int i = 0; i < PRODUCERS; i++)
		pthread_join(th[i], NULL);
	atomic_store(&done, 1);
	pthread_join(dth, NULL);

	CHECK(d.bad == 0);
	CHECK(d.total == per * PRODUCERS);
	for (int i = 0; i < PRODUCERS; i++)
		CHECK(d.next[i] == per);
	(void)ove_trace_ring_dropped_get();
}

int main(void)
{
	test_fifo_order();
	test_partial_drain();
	test_overflow_counts_drops();
	test_wraparound();
	test_concurrent_no_drops_when_room();
	test_concurrent_with_drainer();

	if (failures) {
		fprintf(stderr, "test_trace_ring: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_trace_ring: OK\n");
	return 0;
}
