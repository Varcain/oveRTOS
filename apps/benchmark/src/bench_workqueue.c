/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"

static ove_workqueue_t bench_wq;
static ove_work_t bench_work;
static volatile int work_executed;
static ove_sem_t work_sem;

#ifdef CONFIG_OVE_ZERO_HEAP
OVE_WORKQUEUE_DEFINE(bench_wq_storage, 2048);
OVE_SEM_DEFINE(work_sem_storage);
static ove_work_storage_t bench_work_storage;
#endif

static void work_handler(ove_work_t work)
{
	(void)work;
	work_executed = 1;
	ove_sem_give(work_sem);
}

/* --- create/destroy --- */

#ifndef CONFIG_OVE_ZERO_HEAP
static void wq_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_workqueue_t wq;

	ove_workqueue_create(&wq, "bench_wq", OVE_PRIO_NORMAL, 2048);
	ove_workqueue_destroy(wq);
}
#endif

/* --- submit/execute --- */

static void wq_submit_setup(void *ctx)
{
	(void)ctx;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_sem_init(&work_sem, &work_sem_storage, 0, 1);
	ove_workqueue_init(&bench_wq, &bench_wq_storage, "bench_wq",
			       OVE_PRIO_NORMAL, 2048,
			       bench_wq_storage_stack);
	ove_work_init_static(&bench_work, &bench_work_storage,
				  work_handler);
#else
	ove_sem_create(&work_sem, 0, 1);
	ove_workqueue_create(&bench_wq, "bench_wq", OVE_PRIO_NORMAL,
				 2048);
	ove_work_init(&bench_work, work_handler);
#endif
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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_workqueue_deinit(bench_wq);
	ove_sem_deinit(work_sem);
#else
	ove_work_free(bench_work);
	ove_workqueue_destroy(bench_wq);
	ove_sem_destroy(work_sem);
#endif
}

/* --- memory --- */

#ifndef CONFIG_OVE_ZERO_HEAP
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
#endif

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
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "memory",
		.type = BENCH_TYPE_MEMORY,
		.run = wq_memory_run,
		.teardown = wq_memory_teardown,
	},
	{
		.name = "create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = wq_create_destroy_run,
		.iterations = 200,
	},
#endif
	{
		.name = "submit_execute",
		.type = BENCH_TYPE_LATENCY,
		.setup = wq_submit_setup,
		.run = wq_submit_run,
		.teardown = wq_submit_teardown,
		.iterations = 500,
	},
};

const bench_suite_t bench_suite_workqueue = {
	.name = "workqueue",
	.is_enabled = workqueue_is_enabled,
	.cases = workqueue_cases,
	.case_count = sizeof(workqueue_cases) / sizeof(workqueue_cases[0]),
};
