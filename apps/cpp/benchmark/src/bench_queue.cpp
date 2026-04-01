/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>

extern "C" {
#include "benchmark.h"
}

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

	struct ove_thread_desc desc = {};
	desc.name = "q_prod";
	desc.entry = producer_thread;
	desc.arg = nullptr;
	desc.priority = OVE_PRIO_NORMAL;

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
		"memory",
		BENCH_TYPE_MEMORY,
		nullptr,
		queue_memory_run,
		queue_memory_teardown,
		0,
	},
	{
		"send_receive",
		BENCH_TYPE_LATENCY,
		queue_send_recv_setup,
		queue_send_recv_run,
		queue_send_recv_teardown,
		0,
	},
	{
		"create_destroy",
		BENCH_TYPE_LATENCY,
		nullptr,
		queue_create_destroy_run,
		nullptr,
		0,
	},
	{
		"throughput_2t",
		BENCH_TYPE_THROUGHPUT,
		queue_throughput_setup,
		queue_throughput_run,
		queue_throughput_teardown,
		0,
	},
};

extern "C" const bench_suite_t bench_suite_queue = {
	"queue",
	queue_is_enabled,
	queue_cases,
	sizeof(queue_cases) / sizeof(queue_cases[0]),
};
