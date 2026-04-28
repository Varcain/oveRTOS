/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>
#include <ove/bench.hpp>
#include <cstring>

#include <atomic>
#include <optional>

static constexpr size_t STREAM_BUF_SIZE = 256;
static constexpr size_t STREAM_MSG_SIZE = 64;

using BenchStream = ove::Stream<STREAM_BUF_SIZE>;

static std::optional<BenchStream> bench_strm;
static std::optional<ove::Thread<2048>> stream_producer_th;
static std::atomic<bool> stream_done{false};

static uint8_t tx_buf[STREAM_MSG_SIZE];
static uint8_t rx_buf[STREAM_MSG_SIZE];

/* --- send/receive 64B --- */

static void stream_send_recv_setup()
{
	bench_strm.emplace(1);
	std::memset(tx_buf, 0xAA, STREAM_MSG_SIZE);
}

static void stream_send_recv_run()
{
	size_t sent = 0;
	size_t received = 0;
	(void)bench_strm->send(tx_buf, STREAM_MSG_SIZE, OVE_WAIT_FOREVER, &sent);
	(void)bench_strm->receive(rx_buf, STREAM_MSG_SIZE, OVE_WAIT_FOREVER, &received);
}

static void stream_send_recv_teardown()
{
	bench_strm.reset();
}

/* --- create/destroy --- */

static void stream_create_destroy_run()
{
	BenchStream s(1);
}

/* --- throughput --- */

static void stream_producer(void *arg)
{
	(void)arg;
	while (!stream_done.load(std::memory_order_acquire)) {
		size_t sent = 0;
		(void)bench_strm->send(tx_buf, STREAM_MSG_SIZE, OVE_WAIT_FOREVER, &sent);
	}
}

static void stream_throughput_setup()
{
	stream_done.store(false, std::memory_order_release);
	std::memset(tx_buf, 0xBB, STREAM_MSG_SIZE);
	bench_strm.emplace(1);
	stream_producer_th.emplace(stream_producer, nullptr, OVE_PRIO_NORMAL, "strm_prod");
}

static void stream_throughput_run()
{
	size_t received = 0;
	(void)bench_strm->receive(rx_buf, STREAM_MSG_SIZE, OVE_WAIT_FOREVER, &received);
}

static void stream_throughput_teardown()
{
	stream_done.store(true, std::memory_order_release);
	/* Drain so producer can unblock */
	size_t received = 0;
	(void)bench_strm->receive(rx_buf, STREAM_MSG_SIZE, 100, &received);
	ove::time::delay_ms(10);
	stream_producer_th.reset();
	bench_strm.reset();
}

/* --- memory --- */

static std::optional<BenchStream> mem_stream;

static void stream_memory_run()
{
	mem_stream.emplace(1);
}

static void stream_memory_teardown()
{
	mem_stream.reset();
}

/* --- Suite --- */

static bool stream_is_enabled()
{
	return true;
}

static constexpr ove::bench::CaseSpec stream_memory_spec{
	.name = "memory",
	.kind = ove::bench::Type::memory,
	.run = &stream_memory_run,
	.teardown = &stream_memory_teardown,
};
static constexpr ove::bench::CaseSpec stream_send_recv_spec{
	.name = "send_recv_64B",
	.kind = ove::bench::Type::latency,
	.run = &stream_send_recv_run,
	.setup = &stream_send_recv_setup,
	.teardown = &stream_send_recv_teardown,
};
static constexpr ove::bench::CaseSpec stream_create_destroy_spec{
	.name = "create_destroy",
	.kind = ove::bench::Type::latency,
	.run = &stream_create_destroy_run,
};
static constexpr ove::bench::CaseSpec stream_throughput_spec{
	.name = "throughput",
	.kind = ove::bench::Type::throughput,
	.run = &stream_throughput_run,
	.setup = &stream_throughput_setup,
	.teardown = &stream_throughput_teardown,
};

static constexpr bench_case_t stream_cases[] = {
	ove::bench::case_<stream_memory_spec>(),
	ove::bench::case_<stream_send_recv_spec>(),
	ove::bench::case_<stream_create_destroy_spec>(),
	ove::bench::case_<stream_throughput_spec>(),
};

OVE_BENCH_SUITE(bench_suite_stream, "stream", stream_is_enabled, stream_cases)
