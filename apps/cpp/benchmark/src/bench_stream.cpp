/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>
#include <cstring>

extern "C" {
#include "benchmark.h"
}

static ove_stream_t bench_strm;
static ove_thread_t stream_producer_th;
static volatile int stream_done;

static constexpr size_t STREAM_BUF_SIZE = 256;
static constexpr size_t STREAM_MSG_SIZE = 64;

static uint8_t tx_buf[STREAM_MSG_SIZE];
static uint8_t rx_buf[STREAM_MSG_SIZE];

/* --- send/receive 64B --- */

static void stream_send_recv_setup(void *ctx)
{
	(void)ctx;
	ove_stream_create(&bench_strm, STREAM_BUF_SIZE, 1);
	std::memset(tx_buf, 0xAA, STREAM_MSG_SIZE);
}

static void stream_send_recv_run(void *ctx)
{
	(void)ctx;
	size_t sent = 0;
	size_t received = 0;

	ove_stream_send(bench_strm, tx_buf, STREAM_MSG_SIZE,
			OVE_WAIT_FOREVER, &sent);
	ove_stream_receive(bench_strm, rx_buf, STREAM_MSG_SIZE,
			   OVE_WAIT_FOREVER, &received);
}

static void stream_send_recv_teardown(void *ctx)
{
	(void)ctx;
	ove_stream_destroy(bench_strm);
}

/* --- create/destroy --- */

static void stream_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_stream_t s;

	ove_stream_create(&s, STREAM_BUF_SIZE, 1);
	ove_stream_destroy(s);
}

/* --- throughput --- */

static void stream_producer(void *arg)
{
	(void)arg;

	while (!stream_done) {
		size_t sent = 0;

		ove_stream_send(bench_strm, tx_buf, STREAM_MSG_SIZE,
				OVE_WAIT_FOREVER, &sent);
	}
}

static void stream_throughput_setup(void *ctx)
{
	(void)ctx;
	stream_done = 0;
	std::memset(tx_buf, 0xBB, STREAM_MSG_SIZE);
	ove_stream_create(&bench_strm, STREAM_BUF_SIZE, 1);

	struct ove_thread_desc desc = {};
	desc.name = "strm_prod";
	desc.entry = stream_producer;
	desc.arg = nullptr;
	desc.priority = OVE_PRIO_NORMAL;

	ove_thread_create(&stream_producer_th, 2048, &desc);
}

static void stream_throughput_run(void *ctx)
{
	(void)ctx;
	size_t received = 0;

	ove_stream_receive(bench_strm, rx_buf, STREAM_MSG_SIZE,
			   OVE_WAIT_FOREVER, &received);
}

static void stream_throughput_teardown(void *ctx)
{
	(void)ctx;
	stream_done = 1;
	/* Drain so producer can unblock */
	size_t received = 0;

	ove_stream_receive(bench_strm, rx_buf, STREAM_MSG_SIZE,
			   100, &received);
	ove_thread_sleep_ms(10);
	ove_thread_destroy(stream_producer_th);
	ove_stream_destroy(bench_strm);
}

/* --- memory --- */

static ove_stream_t mem_stream;

static void stream_memory_run(void *ctx)
{
	(void)ctx;
	ove_stream_create(&mem_stream, STREAM_BUF_SIZE, 1);
}

static void stream_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_stream_destroy(mem_stream);
}

/* --- Suite --- */

static int stream_is_enabled(void)
{
	return 1;
}

static const bench_case_t stream_cases[] = {
	{
		"memory",
		BENCH_TYPE_MEMORY,
		nullptr,
		stream_memory_run,
		stream_memory_teardown,
		0,
	},
	{
		"send_recv_64B",
		BENCH_TYPE_LATENCY,
		stream_send_recv_setup,
		stream_send_recv_run,
		stream_send_recv_teardown,
		0,
	},
	{
		"create_destroy",
		BENCH_TYPE_LATENCY,
		nullptr,
		stream_create_destroy_run,
		nullptr,
		0,
	},
	{
		"throughput",
		BENCH_TYPE_THROUGHPUT,
		stream_throughput_setup,
		stream_throughput_run,
		stream_throughput_teardown,
		0,
	},
};

extern "C" const bench_suite_t bench_suite_stream = {
	"stream",
	stream_is_enabled,
	stream_cases,
	sizeof(stream_cases) / sizeof(stream_cases[0]),
};
