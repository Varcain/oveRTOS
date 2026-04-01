/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"

/* --- Shared state --- */

static ove_queue_t bench_q;
static ove_thread_t producer_th;
static volatile int throughput_done;

/* --- send/receive latency --- */

static void queue_send_recv_setup(void *ctx)
{
	(void)ctx;
	ove_queue_create(&bench_q, sizeof(uint32_t), 16);
}

static void queue_send_recv_run(void *ctx)
{
	(void)ctx;
	uint32_t val = 42;
	uint32_t buf;

	ove_queue_send(bench_q, &val, OVE_WAIT_FOREVER);
	ove_queue_receive(bench_q, &buf, OVE_WAIT_FOREVER);
}

static void queue_send_recv_teardown(void *ctx)
{
	(void)ctx;
	ove_queue_destroy(bench_q);
}

/* --- create/destroy --- */

static void queue_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_queue_t q;

	ove_queue_create(&q, sizeof(uint32_t), 8);
	ove_queue_destroy(q);
}

/* --- 2-thread throughput --- */

static void producer_thread(void *arg)
{
	(void)arg;
	uint32_t val = 0;

	while (!throughput_done) {
		ove_queue_send(bench_q, &val, OVE_WAIT_FOREVER);
		val++;
	}
}

static void queue_throughput_setup(void *ctx)
{
	(void)ctx;
	throughput_done = 0;
	ove_queue_create(&bench_q, sizeof(uint32_t), 64);

	struct ove_thread_desc desc = {
		.name = "q_prod",
		.entry = producer_thread,
		.arg = NULL,
		.priority = OVE_PRIO_NORMAL,
	};
	ove_thread_create(&producer_th, 2048, &desc);
}

static void queue_throughput_run(void *ctx)
{
	(void)ctx;
	uint32_t buf;

	ove_queue_receive(bench_q, &buf, OVE_WAIT_FOREVER);
}

static void queue_throughput_teardown(void *ctx)
{
	(void)ctx;
	throughput_done = 1;
	/* Drain queue so producer unblocks */
	uint32_t buf;

	ove_queue_receive(bench_q, &buf, 100);
	ove_thread_sleep_ms(10);
	ove_thread_destroy(producer_th);
	ove_queue_destroy(bench_q);
}

/* --- memory --- */

static ove_queue_t mem_queue;

static void queue_memory_run(void *ctx)
{
	(void)ctx;
	ove_queue_create(&mem_queue, sizeof(uint32_t), 8);
}

static void queue_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_queue_destroy(mem_queue);
}

/* --- Suite --- */

static int queue_is_enabled(void)
{
#ifdef CONFIG_OVE_QUEUE
	return 1;
#else
	return 0;
#endif
}

static const bench_case_t queue_cases[] = {
	{
		.name = "memory",
		.type = BENCH_TYPE_MEMORY,
		.run = queue_memory_run,
		.teardown = queue_memory_teardown,
	},
	{
		.name = "send_receive",
		.type = BENCH_TYPE_LATENCY,
		.setup = queue_send_recv_setup,
		.run = queue_send_recv_run,
		.teardown = queue_send_recv_teardown,
	},
	{
		.name = "create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = queue_create_destroy_run,
	},
	{
		.name = "throughput_2t",
		.type = BENCH_TYPE_THROUGHPUT,
		.setup = queue_throughput_setup,
		.run = queue_throughput_run,
		.teardown = queue_throughput_teardown,
	},
};

const bench_suite_t bench_suite_queue = {
	.name = "queue",
	.is_enabled = queue_is_enabled,
	.cases = queue_cases,
	.case_count = sizeof(queue_cases) / sizeof(queue_cases[0]),
};
