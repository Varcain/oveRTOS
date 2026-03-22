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

static ove_mutex_t bench_mtx;
static ove_sem_t bench_sem;
static ove_event_t bench_evt;
static ove_condvar_t bench_cv;
static ove_mutex_t bench_cv_mtx;
static ove_mutex_t bench_rmtx;
static ove_thread_t contention_th;
static volatile int contention_done;
static volatile uint32_t contention_count;

#ifdef CONFIG_OVE_ZERO_HEAP
OVE_MUTEX_DEFINE(bench_mtx_storage);
OVE_SEM_DEFINE(bench_sem_storage);
OVE_EVENT_DEFINE(bench_evt_storage);
OVE_CONDVAR_DEFINE(bench_cv_storage);
OVE_MUTEX_DEFINE(bench_cv_mtx_storage);
OVE_MUTEX_DEFINE(bench_rmtx_storage);
OVE_THREAD_DEFINE(contention_th_storage, 2048);
OVE_THREAD_DEFINE(evt_th_storage, 1024);
OVE_THREAD_DEFINE(cv_th_storage, 1024);
#endif

/* --- Mutex lock/unlock --- */

static void mutex_lock_unlock_setup(void *ctx)
{
	(void)ctx;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mutex_init(&bench_mtx, &bench_mtx_storage);
#else
	ove_mutex_create(&bench_mtx);
#endif
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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mutex_deinit(bench_mtx);
#else
	ove_mutex_destroy(bench_mtx);
#endif
}

/* --- Mutex create/destroy --- */

#ifndef CONFIG_OVE_ZERO_HEAP
static void mutex_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_mutex_t m;

	ove_mutex_create(&m);
	ove_mutex_destroy(m);
}
#endif

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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mutex_init(&bench_mtx, &bench_mtx_storage);
#else
	ove_mutex_create(&bench_mtx);
#endif

	struct ove_thread_desc desc = {
		.name = "contention",
		.entry = contention_thread,
		.arg = NULL,
		.priority = OVE_PRIO_NORMAL,
		.stack_size = 2048,
#ifdef CONFIG_OVE_ZERO_HEAP
		.stack = contention_th_storage_stack,
#endif
	};
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_init(&contention_th, &contention_th_storage, &desc);
#else
	ove_thread_create(&contention_th, &desc);
#endif
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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_deinit(contention_th);
	ove_mutex_deinit(bench_mtx);
#else
	ove_thread_destroy(contention_th);
	ove_mutex_destroy(bench_mtx);
#endif
}

/* --- Mutex memory --- */

#ifndef CONFIG_OVE_ZERO_HEAP
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
#endif

/* --- Semaphore take/give --- */

static void sem_take_give_setup(void *ctx)
{
	(void)ctx;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_sem_init(&bench_sem, &bench_sem_storage, 1, 1);
#else
	ove_sem_create(&bench_sem, 1, 1);
#endif
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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_sem_deinit(bench_sem);
#else
	ove_sem_destroy(bench_sem);
#endif
}

/* --- Semaphore create/destroy --- */

#ifndef CONFIG_OVE_ZERO_HEAP
static void sem_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_sem_t s;

	ove_sem_create(&s, 0, 1);
	ove_sem_destroy(s);
}
#endif

/* --- Semaphore memory --- */

#ifndef CONFIG_OVE_ZERO_HEAP
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
#endif

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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_event_init(&bench_evt, &bench_evt_storage);
#else
	ove_event_create(&bench_evt);
#endif

	struct ove_thread_desc desc = {
		.name = "evt_sig",
		.entry = evt_signaler,
		.arg = NULL,
		.priority = OVE_PRIO_NORMAL,
		.stack_size = 1024,
#ifdef CONFIG_OVE_ZERO_HEAP
		.stack = evt_th_storage_stack,
#endif
	};
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_init(&evt_th, &evt_th_storage, &desc);
#else
	ove_thread_create(&evt_th, &desc);
#endif
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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_deinit(evt_th);
	ove_event_deinit(bench_evt);
#else
	ove_thread_destroy(evt_th);
	ove_event_destroy(bench_evt);
#endif
}

/* --- Event memory --- */

#ifndef CONFIG_OVE_ZERO_HEAP
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
#endif

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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mutex_init(&bench_cv_mtx, &bench_cv_mtx_storage);
	ove_condvar_init(&bench_cv, &bench_cv_storage);
#else
	ove_mutex_create(&bench_cv_mtx);
	ove_condvar_create(&bench_cv);
#endif

	struct ove_thread_desc desc = {
		.name = "cv_sig",
		.entry = cv_signaler,
		.arg = NULL,
		.priority = OVE_PRIO_NORMAL,
		.stack_size = 1024,
#ifdef CONFIG_OVE_ZERO_HEAP
		.stack = cv_th_storage_stack,
#endif
	};
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_init(&cv_th, &cv_th_storage, &desc);
#else
	ove_thread_create(&cv_th, &desc);
#endif
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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_deinit(cv_th);
	ove_condvar_deinit(bench_cv);
	ove_mutex_deinit(bench_cv_mtx);
#else
	ove_thread_destroy(cv_th);
	ove_condvar_destroy(bench_cv);
	ove_mutex_destroy(bench_cv_mtx);
#endif
}

/* --- Condvar memory --- */

#ifndef CONFIG_OVE_ZERO_HEAP
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
#endif

/* --- Recursive mutex lock/unlock --- */

static void rmtx_lock_unlock_setup(void *ctx)
{
	(void)ctx;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_recursive_mutex_init(&bench_rmtx, &bench_rmtx_storage);
#else
	ove_recursive_mutex_create(&bench_rmtx);
#endif
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
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mutex_deinit(bench_rmtx);
#else
	ove_recursive_mutex_destroy(bench_rmtx);
#endif
}

/* --- Suite --- */

static int sync_is_enabled(void)
{
#ifdef CONFIG_OVE_SYNC
	return 1;
#else
	return 0;
#endif
}

static const bench_case_t sync_cases[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
	/* Memory tests first — before thread-heavy tests affect heap state */
	{
		.name = "mutex_memory",
		.type = BENCH_TYPE_MEMORY,
		.run = mutex_memory_run,
		.teardown = mutex_memory_teardown,
	},
	{
		.name = "sem_memory",
		.type = BENCH_TYPE_MEMORY,
		.run = sem_memory_run,
		.teardown = sem_memory_teardown,
	},
	{
		.name = "event_memory",
		.type = BENCH_TYPE_MEMORY,
		.run = event_memory_run,
		.teardown = event_memory_teardown,
	},
	{
		.name = "condvar_memory",
		.type = BENCH_TYPE_MEMORY,
		.run = condvar_memory_run,
		.teardown = condvar_memory_teardown,
	},
#endif
	{
		.name = "mutex_lock_unlock",
		.type = BENCH_TYPE_LATENCY,
		.setup = mutex_lock_unlock_setup,
		.run = mutex_lock_unlock_run,
		.teardown = mutex_lock_unlock_teardown,
	},
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "mutex_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = mutex_create_destroy_run,
	},
#endif
	{
		.name = "mutex_contention_2t",
		.type = BENCH_TYPE_THROUGHPUT,
		.setup = mutex_contention_setup,
		.run = mutex_contention_run,
		.teardown = mutex_contention_teardown,
	},
	{
		.name = "sem_take_give",
		.type = BENCH_TYPE_LATENCY,
		.setup = sem_take_give_setup,
		.run = sem_take_give_run,
		.teardown = sem_take_give_teardown,
	},
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "sem_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = sem_create_destroy_run,
	},
#endif
	{
		.name = "event_signal_wait",
		.type = BENCH_TYPE_LATENCY,
		.setup = event_signal_wait_setup,
		.run = event_signal_wait_run,
		.teardown = event_signal_wait_teardown,
		.iterations = 500,
	},
	{
		.name = "condvar_signal_wait",
		.type = BENCH_TYPE_LATENCY,
		.setup = condvar_signal_wait_setup,
		.run = condvar_signal_wait_run,
		.teardown = condvar_signal_wait_teardown,
		.iterations = 500,
	},
	{
		.name = "recursive_mutex_lock_unlock",
		.type = BENCH_TYPE_LATENCY,
		.setup = rmtx_lock_unlock_setup,
		.run = rmtx_lock_unlock_run,
		.teardown = rmtx_lock_unlock_teardown,
	},
};

const bench_suite_t bench_suite_sync = {
	.name = "sync",
	.is_enabled = sync_is_enabled,
	.cases = sync_cases,
	.case_count = sizeof(sync_cases) / sizeof(sync_cases[0]),
};
