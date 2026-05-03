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

/* --- Mutex create/destroy ---
 *
 * Heap-mode only: under ZEROHEAP, `ove_mutex_create` macro-expands to
 * `ove_mutex_init` against per-call-site static storage; re-init in a
 * loop is technically valid but measures something different (init+
 * deinit cycle on the same static buffer) than the heap-mode case.
 * Skip the case entirely; the per-call lock/unlock op is what matters
 * for the minimal-overhead claim. */
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
	ove_mutex_create(&bench_mtx);

	ove_thread_create(&contention_th, "contention", contention_thread, NULL, OVE_PRIO_NORMAL,
			  2048);
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

/* --- Mutex memory ---
 *
 * Heap-delta measurement is meaningless under ZEROHEAP (zero by
 * construction).  Gate out; the wrapper's static-storage size is
 * available at compile time via `sizeof(ove_mutex_storage_t)` for
 * users who want it. */
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

/* --- Semaphore create/destroy + memory (heap-mode only — see comments
 * above the mutex variants). */
#ifndef CONFIG_OVE_ZERO_HEAP
static void sem_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_sem_t s;

	ove_sem_create(&s, 0, 1);
	ove_sem_destroy(s);
}

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

static ove_event_t bench_evt_ack;
static ove_thread_t evt_th;
static volatile int evt_done;

static void evt_signaler(void *arg)
{
	(void)arg;

	while (!evt_done) {
		ove_event_signal(bench_evt);
		ove_event_wait(bench_evt_ack, OVE_WAIT_FOREVER);
	}
}

static void event_signal_wait_setup(void *ctx)
{
	(void)ctx;
	evt_done = 0;
	ove_event_create(&bench_evt);
	ove_event_create(&bench_evt_ack);

	ove_thread_create(&evt_th, "evt_sig", evt_signaler, NULL, OVE_PRIO_NORMAL, 1024);
}

static void event_signal_wait_run(void *ctx)
{
	(void)ctx;
	ove_event_wait(bench_evt, OVE_WAIT_FOREVER);
	ove_event_signal(bench_evt_ack);
}

static void event_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	evt_done = 1;
	ove_event_signal(bench_evt_ack);
	ove_thread_sleep_ms(10);
	ove_thread_destroy(evt_th);
	ove_event_destroy(bench_evt);
	ove_event_destroy(bench_evt_ack);
}

/* --- Event memory (heap-mode only). */
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

/* --- Condvar signal/wait ---
 *
 * Condvar uses the original yield-based signaler + bounded cv_wait
 * timeout (10 ms).  An ack-pattern signaler (like event's) deadlocks:
 * condvar's cv->head is *edge-triggered* — a signal fired while no
 * task is registered in the wait list is silently dropped, unlike a
 * task notification which accumulates in a per-task counter.  An
 * iter-aligned signaler can therefore lose its signal in the gap
 * between two run() calls, then block forever waiting for the ack.
 * The 10 ms cv_wait timeout breaks any rare lost-signal cases without
 * adding measurable overhead in the hot path. */

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

	ove_thread_create(&cv_th, "cv_sig", cv_signaler, NULL, OVE_PRIO_NORMAL, 1024);
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

/* --- Condvar memory (heap-mode only). */
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
#ifndef CONFIG_OVE_ZERO_HEAP
	/* Memory tests first — before thread-heavy tests affect heap state.
	 * Heap-mode only — meaningless under ZEROHEAP. */
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
