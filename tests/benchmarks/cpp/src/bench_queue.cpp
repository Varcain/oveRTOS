/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>
#include "ove_bench.hpp"

#include <atomic>
#include <chrono>
#include <optional>

using namespace std::chrono_literals;

/* --- Shared state --- */

using BenchQueue16 = ove::Queue<uint32_t, 16>;
using BenchQueue64 = ove::Queue<uint32_t, 64>;
using BenchQueue8 = ove::Queue<uint32_t, 8>;

static std::optional<BenchQueue16> bench_q;
static std::optional<BenchQueue64> throughput_q;
static std::optional<ove::Thread<2048>> producer_th;
static std::atomic<bool> throughput_done{false};

/* --- send/receive latency --- */

static void queue_send_recv_setup()
{
	bench_q.emplace();
}

static void queue_send_recv_run()
{
	uint32_t val = 42;
	uint32_t buf;
	(void)bench_q->send(val, ove::wait_forever);
	(void)bench_q->receive(&buf, ove::wait_forever);
}

static void queue_send_recv_teardown()
{
	bench_q.reset();
}

/* --- create/destroy (heap-mode only) --- */
#ifndef CONFIG_OVE_ZERO_HEAP
static void queue_create_destroy_run()
{
	BenchQueue8 q;
}
#endif

/* --- 2-thread throughput --- */

static void producer_thread(void *arg)
{
	(void)arg;
	uint32_t val = 0;
	while (!throughput_done.load(std::memory_order_acquire)) {
		(void)throughput_q->send(val, ove::wait_forever);
		val++;
	}
}

static void queue_throughput_setup()
{
	throughput_done.store(false, std::memory_order_release);
	throughput_q.emplace();
	producer_th.emplace(producer_thread, nullptr, OVE_PRIO_NORMAL, "q_prod");
}

static void queue_throughput_run()
{
	uint32_t buf;
	(void)throughput_q->receive(&buf, ove::wait_forever);
}

static void queue_throughput_teardown()
{
	throughput_done.store(true, std::memory_order_release);
	uint32_t buf;
	(void)throughput_q->receive(&buf, 100ms);
	ove::time::delay_ms(10);
	producer_th.reset();
	throughput_q.reset();
}

/* --- memory (heap-mode only) --- */
#ifndef CONFIG_OVE_ZERO_HEAP
static std::optional<BenchQueue8> mem_queue;

static void queue_memory_run()
{
	mem_queue.emplace();
}

static void queue_memory_teardown()
{
	mem_queue.reset();
}
#endif

/* --- Suite --- */

static bool queue_is_enabled()
{
	return true;
}

#ifndef CONFIG_OVE_ZERO_HEAP
static constexpr bench::CaseSpec queue_memory_spec{
	.name = "memory",
	.kind = bench::Type::memory,
	.run = &queue_memory_run,
	.teardown = &queue_memory_teardown,
};
#endif
static constexpr bench::CaseSpec queue_send_recv_spec{
	.name = "send_receive",
	.kind = bench::Type::latency,
	.run = &queue_send_recv_run,
	.setup = &queue_send_recv_setup,
	.teardown = &queue_send_recv_teardown,
};
#ifndef CONFIG_OVE_ZERO_HEAP
static constexpr bench::CaseSpec queue_create_destroy_spec{
	.name = "create_destroy",
	.kind = bench::Type::latency,
	.run = &queue_create_destroy_run,
};
#endif
static constexpr bench::CaseSpec queue_throughput_spec{
	.name = "throughput_2t",
	.kind = bench::Type::throughput,
	.run = &queue_throughput_run,
	.setup = &queue_throughput_setup,
	.teardown = &queue_throughput_teardown,
};

static constexpr bench_case_t queue_cases[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
	bench::case_<queue_memory_spec>(),
#endif
	bench::case_<queue_send_recv_spec>(),
#ifndef CONFIG_OVE_ZERO_HEAP
	bench::case_<queue_create_destroy_spec>(),
#endif
	bench::case_<queue_throughput_spec>(),
};

OVE_BENCH_SUITE(bench_suite_queue, "queue", queue_is_enabled, queue_cases)
