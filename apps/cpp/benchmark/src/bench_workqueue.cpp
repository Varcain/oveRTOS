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

static ove_workqueue_t bench_wq;
static ove_work_t bench_work;
static volatile int work_executed;
static ove_sem_t work_sem;

static ove_work_storage_t bench_work_storage;

static void work_handler(ove_work_t work)
{
	(void)work;
	work_executed = 1;
	ove_sem_give(work_sem);
}

/* --- create/destroy --- */

static void wq_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_workqueue_t wq;

	ove_workqueue_create(&wq, "bench_wq", OVE_PRIO_NORMAL, 2048);
	ove_workqueue_destroy(wq);
}

/* --- submit/execute --- */

static void wq_submit_setup(void *ctx)
{
	(void)ctx;
	ove_sem_create(&work_sem, 0, 1);
	ove_workqueue_create(&bench_wq, "bench_wq", OVE_PRIO_NORMAL, 2048);
	ove_work_init_static(&bench_work, &bench_work_storage, work_handler);
}

static void wq_submit_run(void *ctx)
{
	(void)ctx;
	work_executed = 0;
	ove_work_submit(bench_wq, bench_work);
	ove_sem_take(work_sem, 1000);
}

static void wq_submit_teardown(void *ctx)
{
	(void)ctx;
	ove_workqueue_destroy(bench_wq);
	ove_sem_destroy(work_sem);
}

/* --- memory --- */

static ove_workqueue_t mem_wq;

static void wq_memory_run(void *ctx)
{
	(void)ctx;
	ove_workqueue_create(&mem_wq, "bench_wq", OVE_PRIO_NORMAL, 2048);
}

static void wq_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_workqueue_destroy(mem_wq);
}

/* --- Suite --- */

static int workqueue_is_enabled(void)
{
#ifdef CONFIG_OVE_WORKQUEUE
	return 1;
#else
	return 0;
#endif
}

static const bench_case_t workqueue_cases[] = {
	{
		"memory",
		BENCH_TYPE_MEMORY,
		nullptr,
		wq_memory_run,
		wq_memory_teardown,
		0,
	},
	{
		"create_destroy",
		BENCH_TYPE_LATENCY,
		nullptr,
		wq_create_destroy_run,
		nullptr,
		200,
	},
	{
		"submit_execute",
		BENCH_TYPE_LATENCY,
		wq_submit_setup,
		wq_submit_run,
		wq_submit_teardown,
		500,
	},
};

extern "C" const bench_suite_t bench_suite_workqueue = {
	"workqueue",
	workqueue_is_enabled,
	workqueue_cases,
	sizeof(workqueue_cases) / sizeof(workqueue_cases[0]),
};
