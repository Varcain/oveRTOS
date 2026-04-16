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

static ove_mutex_t bench_mtx;
static ove_sem_t bench_sem;
static ove_event_t bench_evt;
static ove_condvar_t bench_cv;
static ove_mutex_t bench_cv_mtx;
static ove_mutex_t bench_rmtx;
static ove_thread_t contention_th;
static volatile int contention_done;
static volatile uint32_t contention_count;

/* --- Mutex lock/unlock --- */

static void mutex_lock_unlock_setup(void *ctx)
{
	(void)ctx;
	ove_mutex_create(&bench_mtx);
}

static void mutex_lock_unlock_run(void *ctx)
{
	(void)ctx;
	ove_mutex_lock(bench_mtx, OVE_WAIT_FOREVER);
	ove_mutex_unlock(bench_mtx);
}

static void mutex_lock_unlock_teardown(void *ctx)
{
	(void)ctx;
	ove_mutex_destroy(bench_mtx);
}

/* --- Mutex create/destroy --- */

static void mutex_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_mutex_t m;

	ove_mutex_create(&m);
	ove_mutex_destroy(m);
}

/* --- Mutex contention (2-thread throughput) --- */

static void contention_thread(void *arg)
{
	(void)arg;

	while (!contention_done) {
		ove_mutex_lock(bench_mtx, OVE_WAIT_FOREVER);
		contention_count++;
		ove_mutex_unlock(bench_mtx);
	}
}

static void mutex_contention_setup(void *ctx)
{
	(void)ctx;
	contention_done = 0;
	contention_count = 0;
	ove_mutex_create(&bench_mtx);

	struct ove_thread_desc desc = {};
	desc.name = "contention";
	desc.entry = contention_thread;
	desc.arg = nullptr;
	desc.priority = OVE_PRIO_NORMAL;

	ove_thread_create(&contention_th, 2048, &desc);
}

static void mutex_contention_run(void *ctx)
{
	(void)ctx;
	ove_mutex_lock(bench_mtx, OVE_WAIT_FOREVER);
	contention_count++;
	ove_mutex_unlock(bench_mtx);
}

static void mutex_contention_teardown(void *ctx)
{
	(void)ctx;
	contention_done = 1;
	ove_thread_sleep_ms(10);
	ove_thread_destroy(contention_th);
	ove_mutex_destroy(bench_mtx);
}

/* --- Mutex memory --- */

static ove_mutex_t mem_mutex;

static void mutex_memory_run(void *ctx)
{
	(void)ctx;
	ove_mutex_create(&mem_mutex);
}

static void mutex_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_mutex_destroy(mem_mutex);
}

/* --- Semaphore take/give --- */

static void sem_take_give_setup(void *ctx)
{
	(void)ctx;
	ove_sem_create(&bench_sem, 1, 1);
}

static void sem_take_give_run(void *ctx)
{
	(void)ctx;
	ove_sem_take(bench_sem, OVE_WAIT_FOREVER);
	ove_sem_give(bench_sem);
}

static void sem_take_give_teardown(void *ctx)
{
	(void)ctx;
	ove_sem_destroy(bench_sem);
}

/* --- Semaphore create/destroy --- */

static void sem_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_sem_t s;

	ove_sem_create(&s, 0, 1);
	ove_sem_destroy(s);
}

/* --- Semaphore memory --- */

static ove_sem_t mem_sem;

static void sem_memory_run(void *ctx)
{
	(void)ctx;
	ove_sem_create(&mem_sem, 0, 1);
}

static void sem_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_sem_destroy(mem_sem);
}

/* --- Event signal/wait --- */

static ove_thread_t evt_th;
static volatile int evt_done;

static void evt_signaler(void *arg)
{
	(void)arg;

	while (!evt_done) {
		ove_event_signal(bench_evt);
		ove_thread_yield();
	}
}

static void event_signal_wait_setup(void *ctx)
{
	(void)ctx;
	evt_done = 0;
	ove_event_create(&bench_evt);

	struct ove_thread_desc desc = {};
	desc.name = "evt_sig";
	desc.entry = evt_signaler;
	desc.arg = nullptr;
	desc.priority = OVE_PRIO_NORMAL;

	ove_thread_create(&evt_th, 1024, &desc);
}

static void event_signal_wait_run(void *ctx)
{
	(void)ctx;
	ove_event_wait(bench_evt, 10);
}

static void event_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	evt_done = 1;
	ove_thread_sleep_ms(10);
	ove_thread_destroy(evt_th);
	ove_event_destroy(bench_evt);
}

/* --- Event memory --- */

static ove_event_t mem_event;

static void event_memory_run(void *ctx)
{
	(void)ctx;
	ove_event_create(&mem_event);
}

static void event_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_event_destroy(mem_event);
}

/* --- Condvar signal/wait --- */

static ove_thread_t cv_th;
static volatile int cv_done;

static void cv_signaler(void *arg)
{
	(void)arg;

	while (!cv_done) {
		ove_condvar_signal(bench_cv);
		ove_thread_yield();
	}
}

static void condvar_signal_wait_setup(void *ctx)
{
	(void)ctx;
	cv_done = 0;
	ove_mutex_create(&bench_cv_mtx);
	ove_condvar_create(&bench_cv);

	struct ove_thread_desc desc = {};
	desc.name = "cv_sig";
	desc.entry = cv_signaler;
	desc.arg = nullptr;
	desc.priority = OVE_PRIO_NORMAL;

	ove_thread_create(&cv_th, 1024, &desc);
}

static void condvar_signal_wait_run(void *ctx)
{
	(void)ctx;
	ove_mutex_lock(bench_cv_mtx, OVE_WAIT_FOREVER);
	ove_condvar_wait(bench_cv, bench_cv_mtx, 10);
	ove_mutex_unlock(bench_cv_mtx);
}

static void condvar_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	cv_done = 1;
	ove_condvar_signal(bench_cv);
	ove_thread_sleep_ms(10);
	ove_thread_destroy(cv_th);
	ove_condvar_destroy(bench_cv);
	ove_mutex_destroy(bench_cv_mtx);
}

/* --- Condvar memory --- */

static ove_condvar_t mem_condvar;

static void condvar_memory_run(void *ctx)
{
	(void)ctx;
	ove_condvar_create(&mem_condvar);
}

static void condvar_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_condvar_destroy(mem_condvar);
}

/* --- Recursive mutex lock/unlock --- */

static void rmtx_lock_unlock_setup(void *ctx)
{
	(void)ctx;
	ove_recursive_mutex_create(&bench_rmtx);
}

static void rmtx_lock_unlock_run(void *ctx)
{
	(void)ctx;
	ove_recursive_mutex_lock(bench_rmtx, OVE_WAIT_FOREVER);
	ove_recursive_mutex_unlock(bench_rmtx);
}

static void rmtx_lock_unlock_teardown(void *ctx)
{
	(void)ctx;
	ove_recursive_mutex_destroy(bench_rmtx);
}

/* --- Suite --- */

static int sync_is_enabled(void)
{
	return 1;
}

static const bench_case_t sync_cases[] = {
	/* Memory tests first -- before thread-heavy tests affect heap state */
	{
		"mutex_memory",
		BENCH_TYPE_MEMORY,
		nullptr,
		mutex_memory_run,
		mutex_memory_teardown,
		0,
	},
	{
		"sem_memory",
		BENCH_TYPE_MEMORY,
		nullptr,
		sem_memory_run,
		sem_memory_teardown,
		0,
	},
	{
		"event_memory",
		BENCH_TYPE_MEMORY,
		nullptr,
		event_memory_run,
		event_memory_teardown,
		0,
	},
	{
		"condvar_memory",
		BENCH_TYPE_MEMORY,
		nullptr,
		condvar_memory_run,
		condvar_memory_teardown,
		0,
	},
	{
		"mutex_lock_unlock",
		BENCH_TYPE_LATENCY,
		mutex_lock_unlock_setup,
		mutex_lock_unlock_run,
		mutex_lock_unlock_teardown,
		0,
	},
	{
		"mutex_create_destroy",
		BENCH_TYPE_LATENCY,
		nullptr,
		mutex_create_destroy_run,
		nullptr,
		0,
	},
	{
		"mutex_contention_2t",
		BENCH_TYPE_THROUGHPUT,
		mutex_contention_setup,
		mutex_contention_run,
		mutex_contention_teardown,
		0,
	},
	{
		"sem_take_give",
		BENCH_TYPE_LATENCY,
		sem_take_give_setup,
		sem_take_give_run,
		sem_take_give_teardown,
		0,
	},
	{
		"sem_create_destroy",
		BENCH_TYPE_LATENCY,
		nullptr,
		sem_create_destroy_run,
		nullptr,
		0,
	},
	{
		"event_signal_wait",
		BENCH_TYPE_LATENCY,
		event_signal_wait_setup,
		event_signal_wait_run,
		event_signal_wait_teardown,
		500,
	},
	{
		"condvar_signal_wait",
		BENCH_TYPE_LATENCY,
		condvar_signal_wait_setup,
		condvar_signal_wait_run,
		condvar_signal_wait_teardown,
		500,
	},
	{
		"recursive_mutex_lock_unlock",
		BENCH_TYPE_LATENCY,
		rmtx_lock_unlock_setup,
		rmtx_lock_unlock_run,
		rmtx_lock_unlock_teardown,
		0,
	},
};

extern "C" const bench_suite_t bench_suite_sync = {
	"sync",
	sync_is_enabled,
	sync_cases,
	sizeof(sync_cases) / sizeof(sync_cases[0]),
};
