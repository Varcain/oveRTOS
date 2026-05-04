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
#include <optional>

/* --- Context for thread benchmarks --- */

static std::optional<ove::Thread<2048>> bench_th;
static std::optional<ove::Semaphore> ping_sem;
static std::optional<ove::Semaphore> pong_sem;
static std::atomic<bool> ctx_switch_done{false};

/* --- create/destroy (heap-mode only) --- */
#ifndef CONFIG_OVE_ZERO_HEAP
static void dummy_thread(void *arg)
{
	(void)arg;
}

static void thread_create_destroy_run()
{
	ove::Thread<1024> th(dummy_thread, nullptr, OVE_PRIO_LOW, "bench_tmp");
}
#endif

/* --- yield --- */

static void thread_yield_run()
{
	ove::Thread<>::yield();
}

/* --- get_self ---
 * Pure "who am I?" query — kernel-side TLS read with no scheduling
 * side-effects.  Distinct from time_get_us_overhead and yield. */
static void thread_get_self_run()
{
	[[maybe_unused]] volatile auto self = ove::Thread<>::self();
}

/* --- sleep 1ms --- */

static void thread_sleep_1ms_run()
{
	ove::Thread<>::sleep_ms(1);
}

/* --- context switch via semaphore ping-pong --- */

static void pong_thread(void *arg)
{
	(void)arg;
	while (!ctx_switch_done.load(std::memory_order_acquire)) {
		(void)ping_sem->take(OVE_WAIT_FOREVER);
		pong_sem->give();
	}
}

static void ctx_switch_setup()
{
	ctx_switch_done.store(false, std::memory_order_release);
	ping_sem.emplace(0, 1);
	pong_sem.emplace(0, 1);
	bench_th.emplace(pong_thread, nullptr, OVE_PRIO_NORMAL, "pong");
}

static void ctx_switch_run()
{
	/* One round-trip = 2 context switches */
	ping_sem->give();
	(void)pong_sem->take(OVE_WAIT_FOREVER);
}

static void ctx_switch_teardown()
{
	ctx_switch_done.store(true, std::memory_order_release);
	ping_sem->give();
	ove::time::delay_ms(10);
	bench_th.reset();
	ping_sem.reset();
	pong_sem.reset();
}

/* --- Suite --- */

static bool thread_is_enabled()
{
	return true;
}

#ifndef CONFIG_OVE_ZERO_HEAP
static constexpr bench::CaseSpec thread_create_destroy_spec{
	.name = "create_destroy",
	.kind = bench::Type::latency,
	.run = &thread_create_destroy_run,
	.iterations = 200,
};
#endif
static constexpr bench::CaseSpec thread_yield_spec{
	.name = "yield",
	.kind = bench::Type::latency,
	.run = &thread_yield_run,
};
static constexpr bench::CaseSpec thread_get_self_spec{
	.name = "get_self",
	.kind = bench::Type::latency,
	.run = &thread_get_self_run,
};
static constexpr bench::CaseSpec thread_sleep_1ms_spec{
	.name = "sleep_1ms",
	.kind = bench::Type::latency,
	.run = &thread_sleep_1ms_run,
	.iterations = 100,
};
static constexpr bench::CaseSpec ctx_switch_spec{
	.name = "context_switch",
	.kind = bench::Type::latency,
	.run = &ctx_switch_run,
	.setup = &ctx_switch_setup,
	.teardown = &ctx_switch_teardown,
	.iterations = 500,
};

static constexpr bench_case_t thread_cases[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
	bench::case_<thread_create_destroy_spec>(),
#endif
	bench::case_<thread_yield_spec>(),	    bench::case_<thread_get_self_spec>(),
	bench::case_<thread_sleep_1ms_spec>(),	    bench::case_<ctx_switch_spec>(),
};

OVE_BENCH_SUITE(bench_suite_thread, "thread", thread_is_enabled, thread_cases)
