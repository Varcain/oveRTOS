/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>
#include <ove/bench.hpp>

#include <atomic>
#include <optional>

using BenchWQ = ove::Workqueue<2048>;

static std::optional<BenchWQ> bench_wq;
static std::optional<ove::Work> bench_work;
static std::optional<ove::Semaphore> work_sem;
static std::atomic<bool> work_executed{false};

static void work_handler(ove_work_t work)
{
	(void)work;
	work_executed.store(true, std::memory_order_release);
	work_sem->give();
}

/* --- create/destroy --- */

static void wq_create_destroy_run()
{
	BenchWQ wq("bench_wq", OVE_PRIO_NORMAL);
}

/* --- submit/execute --- */

static void wq_submit_setup()
{
	work_sem.emplace(0, 1);
	bench_wq.emplace("bench_wq", OVE_PRIO_NORMAL);
	bench_work.emplace(work_handler);
}

static void wq_submit_run()
{
	work_executed.store(false, std::memory_order_release);
	(void)bench_work->submit(*bench_wq);
	(void)work_sem->take(1000);
}

static void wq_submit_teardown()
{
	bench_work.reset();
	bench_wq.reset();
	work_sem.reset();
}

/* --- memory --- */

static std::optional<BenchWQ> mem_wq;

static void wq_memory_run()
{
	mem_wq.emplace("bench_wq", OVE_PRIO_NORMAL);
}

static void wq_memory_teardown()
{
	mem_wq.reset();
}

/* --- Suite --- */

static bool workqueue_is_enabled()
{
	return true;
}

static constexpr ove::bench::CaseSpec wq_memory_spec{
	.name = "memory",
	.kind = ove::bench::Type::memory,
	.run = &wq_memory_run,
	.teardown = &wq_memory_teardown,
};
static constexpr ove::bench::CaseSpec wq_create_destroy_spec{
	.name = "create_destroy",
	.kind = ove::bench::Type::latency,
	.run = &wq_create_destroy_run,
	.iterations = 200,
};
static constexpr ove::bench::CaseSpec wq_submit_spec{
	.name = "submit_execute",
	.kind = ove::bench::Type::latency,
	.run = &wq_submit_run,
	.setup = &wq_submit_setup,
	.teardown = &wq_submit_teardown,
	.iterations = 500,
};

static constexpr bench_case_t workqueue_cases[] = {
	ove::bench::case_<wq_memory_spec>(),
	ove::bench::case_<wq_create_destroy_spec>(),
	ove::bench::case_<wq_submit_spec>(),
};

OVE_BENCH_SUITE(bench_suite_workqueue, "workqueue", workqueue_is_enabled, workqueue_cases)
