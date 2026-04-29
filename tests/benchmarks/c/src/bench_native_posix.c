/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Native pthread baseline — bypasses oveRTOS entirely so the comparison
 * report can show "<binding> wrapper vs raw POSIX API: 0 ns delta within
 * 95% CI".  Only meaningful on the POSIX backend; everywhere else the
 * suite reports as disabled.
 *
 * Cases here MUST mirror operations measured in bench_thread.c,
 * bench_sync.c, bench_queue.c, and bench_stream.c so
 * scripts/bench_compare.py can join them by case stem.  Naming
 * convention: native_<wrapped_case_name>.  Cases without a meaningful
 * raw-POSIX equivalent (event groups, workqueues — not POSIX
 * primitives) are intentionally absent and noted in the report.
 */

#include "benchmark.h"
#include "ove/ove.h"

#if defined(CONFIG_OVE_RTOS_POSIX)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ─── Shared state ─────────────────────────────────────────────── */

static pthread_mutex_t native_mtx;
static pthread_mutex_t native_rmtx;
static sem_t native_sem;
static pthread_cond_t native_cv;
static pthread_mutex_t native_cv_mtx;

/* Event-style flag sat behind a cv+mutex pair (POSIX has no native
 * "event" primitive — pthread_cond is the canonical equivalent). */
static bool native_event_flag;

/* Pipes used as a queue/stream baseline. oveRTOS queue/stream are
 * user-space ring buffers so the comparison is "wrapper user-space
 * ring vs kernel pipe syscall round-trip" — apples-to-orangutans on
 * absolute timing, but it's the closest standard-POSIX IPC primitive. */
static int native_pipe_fd[2] = { -1, -1 };

/* ─── Mutex: lock/unlock ───────────────────────────────────────── */

static void native_mutex_lock_unlock_setup(void *ctx)
{
	(void)ctx;
	pthread_mutex_init(&native_mtx, NULL);
}

static void native_mutex_lock_unlock_run(void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&native_mtx);
	pthread_mutex_unlock(&native_mtx);
}

static void native_mutex_lock_unlock_teardown(void *ctx)
{
	(void)ctx;
	pthread_mutex_destroy(&native_mtx);
}

/* ─── Mutex: create/destroy ────────────────────────────────────── */

static void native_mutex_create_destroy_run(void *ctx)
{
	(void)ctx;
	pthread_mutex_t m;
	pthread_mutex_init(&m, NULL);
	pthread_mutex_destroy(&m);
}

/* ─── Mutex: 2-thread contention throughput ─────────────────────── */

static volatile int contention_done;
static volatile uint32_t contention_count;
static pthread_t contention_th;

static void *native_contention_thread(void *arg)
{
	(void)arg;
	while (!contention_done) {
		pthread_mutex_lock(&native_mtx);
		contention_count++;
		pthread_mutex_unlock(&native_mtx);
	}
	return NULL;
}

static void native_mutex_contention_setup(void *ctx)
{
	(void)ctx;
	contention_done = 0;
	contention_count = 0;
	pthread_mutex_init(&native_mtx, NULL);
	pthread_create(&contention_th, NULL, native_contention_thread, NULL);
}

static void native_mutex_contention_run(void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&native_mtx);
	contention_count++;
	pthread_mutex_unlock(&native_mtx);
}

static void native_mutex_contention_teardown(void *ctx)
{
	(void)ctx;
	contention_done = 1;
	pthread_join(contention_th, NULL);
	pthread_mutex_destroy(&native_mtx);
}

/* ─── Recursive mutex: lock/unlock ─────────────────────────────── */

static void native_recursive_mutex_lock_unlock_setup(void *ctx)
{
	(void)ctx;
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&native_rmtx, &attr);
	pthread_mutexattr_destroy(&attr);
}

static void native_recursive_mutex_lock_unlock_run(void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&native_rmtx);
	pthread_mutex_lock(&native_rmtx);
	pthread_mutex_unlock(&native_rmtx);
	pthread_mutex_unlock(&native_rmtx);
}

static void native_recursive_mutex_lock_unlock_teardown(void *ctx)
{
	(void)ctx;
	pthread_mutex_destroy(&native_rmtx);
}

/* ─── Semaphore: take/give ─────────────────────────────────────── */

static void native_sem_take_give_setup(void *ctx)
{
	(void)ctx;
	sem_init(&native_sem, 0, 1);
}

static void native_sem_take_give_run(void *ctx)
{
	(void)ctx;
	sem_wait(&native_sem);
	sem_post(&native_sem);
}

static void native_sem_take_give_teardown(void *ctx)
{
	(void)ctx;
	sem_destroy(&native_sem);
}

/* ─── Semaphore: create/destroy ────────────────────────────────── */

static void native_sem_create_destroy_run(void *ctx)
{
	(void)ctx;
	sem_t s;
	sem_init(&s, 0, 0);
	sem_destroy(&s);
}

/* ─── Condvar: signal/wait round trip ──────────────────────────── */

static pthread_t native_cv_signaller;
static volatile int native_cv_done;

static void *native_cv_signal_thread(void *arg)
{
	(void)arg;
	while (!native_cv_done) {
		pthread_mutex_lock(&native_cv_mtx);
		while (native_event_flag && !native_cv_done)
			pthread_cond_wait(&native_cv, &native_cv_mtx);
		native_event_flag = true;
		pthread_cond_signal(&native_cv);
		pthread_mutex_unlock(&native_cv_mtx);
	}
	return NULL;
}

static void native_condvar_signal_wait_setup(void *ctx)
{
	(void)ctx;
	pthread_mutex_init(&native_cv_mtx, NULL);
	pthread_cond_init(&native_cv, NULL);
	native_event_flag = false;
	native_cv_done = 0;
	pthread_create(&native_cv_signaller, NULL, native_cv_signal_thread, NULL);
}

static void native_condvar_signal_wait_run(void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&native_cv_mtx);
	while (!native_event_flag)
		pthread_cond_wait(&native_cv, &native_cv_mtx);
	native_event_flag = false;
	pthread_cond_signal(&native_cv);
	pthread_mutex_unlock(&native_cv_mtx);
}

static void native_condvar_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&native_cv_mtx);
	native_cv_done = 1;
	pthread_cond_broadcast(&native_cv);
	pthread_mutex_unlock(&native_cv_mtx);
	pthread_join(native_cv_signaller, NULL);
	pthread_cond_destroy(&native_cv);
	pthread_mutex_destroy(&native_cv_mtx);
}

/* ─── Event: signal/wait round trip ──────────────────────────────
 *
 * POSIX has no native "event" primitive — the canonical equivalent is
 * a bool guarded by pthread_mutex + pthread_cond.  Same machinery as
 * condvar_signal_wait above; we keep both rows so the report can show
 * how oveRTOS event vs oveRTOS condvar compares against the same
 * underlying POSIX primitive.
 */

static pthread_mutex_t native_evt_mtx;
static pthread_cond_t native_evt_cv;
static volatile bool native_evt_flag;
static pthread_t native_evt_signaller;
static volatile int native_evt_done;

static void *native_event_signal_thread(void *arg)
{
	(void)arg;
	while (!native_evt_done) {
		pthread_mutex_lock(&native_evt_mtx);
		while (native_evt_flag && !native_evt_done)
			pthread_cond_wait(&native_evt_cv, &native_evt_mtx);
		native_evt_flag = true;
		pthread_cond_signal(&native_evt_cv);
		pthread_mutex_unlock(&native_evt_mtx);
	}
	return NULL;
}

static void native_event_signal_wait_setup(void *ctx)
{
	(void)ctx;
	pthread_mutex_init(&native_evt_mtx, NULL);
	pthread_cond_init(&native_evt_cv, NULL);
	native_evt_flag = false;
	native_evt_done = 0;
	pthread_create(&native_evt_signaller, NULL, native_event_signal_thread, NULL);
}

static void native_event_signal_wait_run(void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&native_evt_mtx);
	while (!native_evt_flag)
		pthread_cond_wait(&native_evt_cv, &native_evt_mtx);
	native_evt_flag = false;
	pthread_cond_signal(&native_evt_cv);
	pthread_mutex_unlock(&native_evt_mtx);
}

static void native_event_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&native_evt_mtx);
	native_evt_done = 1;
	pthread_cond_broadcast(&native_evt_cv);
	pthread_mutex_unlock(&native_evt_mtx);
	pthread_join(native_evt_signaller, NULL);
	pthread_cond_destroy(&native_evt_cv);
	pthread_mutex_destroy(&native_evt_mtx);
}

/* ─── Thread: yield ────────────────────────────────────────────── */

static void native_thread_yield_run(void *ctx)
{
	(void)ctx;
	sched_yield();
}

/* ─── Thread: sleep_1ms ────────────────────────────────────────── */

static void native_thread_sleep_1ms_run(void *ctx)
{
	(void)ctx;
	struct timespec req = { .tv_sec = 0, .tv_nsec = 1000000L };
	nanosleep(&req, NULL);
}

/* ─── Thread: create/destroy ──────────────────────────────────── */

static void *native_thread_noop(void *arg)
{
	(void)arg;
	return NULL;
}

static void native_thread_create_destroy_run(void *ctx)
{
	(void)ctx;
	pthread_t th;
	pthread_create(&th, NULL, native_thread_noop, NULL);
	pthread_join(th, NULL);
}

/* ─── Thread: context_switch (2-thread ping-pong via 2 sems) ────── */

static sem_t native_cs_a, native_cs_b;
static pthread_t native_cs_thread;
static volatile int native_cs_done;

static void *native_cs_pong_thread(void *arg)
{
	(void)arg;
	while (!native_cs_done) {
		sem_wait(&native_cs_a);
		if (native_cs_done)
			break;
		sem_post(&native_cs_b);
	}
	return NULL;
}

static void native_thread_context_switch_setup(void *ctx)
{
	(void)ctx;
	sem_init(&native_cs_a, 0, 0);
	sem_init(&native_cs_b, 0, 0);
	native_cs_done = 0;
	pthread_create(&native_cs_thread, NULL, native_cs_pong_thread, NULL);
}

static void native_thread_context_switch_run(void *ctx)
{
	(void)ctx;
	sem_post(&native_cs_a);
	sem_wait(&native_cs_b);
}

static void native_thread_context_switch_teardown(void *ctx)
{
	(void)ctx;
	native_cs_done = 1;
	sem_post(&native_cs_a); /* unblock the pong thread so it can exit */
	pthread_join(native_cs_thread, NULL);
	sem_destroy(&native_cs_a);
	sem_destroy(&native_cs_b);
}

/* ─── IPC pipe: send/receive (single byte) ──────────────────────── */

static void native_pipe_create(void)
{
	if (pipe(native_pipe_fd) != 0) {
		native_pipe_fd[0] = native_pipe_fd[1] = -1;
	}
}

static void native_pipe_close(void)
{
	if (native_pipe_fd[0] >= 0)
		close(native_pipe_fd[0]);
	if (native_pipe_fd[1] >= 0)
		close(native_pipe_fd[1]);
	native_pipe_fd[0] = native_pipe_fd[1] = -1;
}

static void native_queue_send_receive_setup(void *ctx)
{
	(void)ctx;
	native_pipe_create();
}

static void native_queue_send_receive_run(void *ctx)
{
	(void)ctx;
	uint8_t buf = 42;
	ssize_t w = write(native_pipe_fd[1], &buf, 1);
	(void)w;
	ssize_t r = read(native_pipe_fd[0], &buf, 1);
	(void)r;
}

static void native_queue_send_receive_teardown(void *ctx)
{
	(void)ctx;
	native_pipe_close();
}

/* ─── IPC pipe: create/destroy ─────────────────────────────────── */

static void native_queue_create_destroy_run(void *ctx)
{
	(void)ctx;
	int fd[2];
	if (pipe(fd) == 0) {
		close(fd[0]);
		close(fd[1]);
	}
}

/* ─── IPC pipe: stream send/recv 64B ───────────────────────────── */

static void native_stream_send_recv_64B_run(void *ctx)
{
	(void)ctx;
	uint8_t buf[64] = { 0 };
	ssize_t w = write(native_pipe_fd[1], buf, sizeof(buf));
	(void)w;
	ssize_t r = read(native_pipe_fd[0], buf, sizeof(buf));
	(void)r;
}

/* ─── Suite registration ─────────────────────────────────────── */

static int native_posix_is_enabled(void)
{
	return 1;
}

static const bench_case_t native_posix_cases[] = {
	/* Mutex */
	{
		.name = "native_mutex_lock_unlock",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_mutex_lock_unlock_setup,
		.run = native_mutex_lock_unlock_run,
		.teardown = native_mutex_lock_unlock_teardown,
	},
	{
		.name = "native_mutex_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = native_mutex_create_destroy_run,
	},
	{
		.name = "native_mutex_contention_2t",
		.type = BENCH_TYPE_THROUGHPUT,
		.setup = native_mutex_contention_setup,
		.run = native_mutex_contention_run,
		.teardown = native_mutex_contention_teardown,
	},
	{
		.name = "native_recursive_mutex_lock_unlock",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_recursive_mutex_lock_unlock_setup,
		.run = native_recursive_mutex_lock_unlock_run,
		.teardown = native_recursive_mutex_lock_unlock_teardown,
	},
	/* Semaphore */
	{
		.name = "native_sem_take_give",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_sem_take_give_setup,
		.run = native_sem_take_give_run,
		.teardown = native_sem_take_give_teardown,
	},
	{
		.name = "native_sem_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = native_sem_create_destroy_run,
	},
	/* Condvar / event */
	{
		.name = "native_condvar_signal_wait",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_condvar_signal_wait_setup,
		.run = native_condvar_signal_wait_run,
		.teardown = native_condvar_signal_wait_teardown,
		.iterations = 500,
	},
	{
		.name = "native_event_signal_wait",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_event_signal_wait_setup,
		.run = native_event_signal_wait_run,
		.teardown = native_event_signal_wait_teardown,
		.iterations = 500,
	},
	/* Threading */
	{
		.name = "native_thread_yield",
		.type = BENCH_TYPE_LATENCY,
		.run = native_thread_yield_run,
	},
	{
		.name = "native_thread_sleep_1ms",
		.type = BENCH_TYPE_LATENCY,
		.run = native_thread_sleep_1ms_run,
		.iterations = 100,
	},
	{
		.name = "native_thread_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = native_thread_create_destroy_run,
		.iterations = 200,
	},
	{
		.name = "native_thread_context_switch",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_thread_context_switch_setup,
		.run = native_thread_context_switch_run,
		.teardown = native_thread_context_switch_teardown,
		.iterations = 500,
	},
	/* IPC (pipe-based proxy for queue / stream) */
	{
		.name = "native_queue_send_receive",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_queue_send_receive_setup,
		.run = native_queue_send_receive_run,
		.teardown = native_queue_send_receive_teardown,
	},
	{
		.name = "native_queue_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = native_queue_create_destroy_run,
	},
	{
		.name = "native_stream_send_recv_64B",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_queue_send_receive_setup,
		.run = native_stream_send_recv_64B_run,
		.teardown = native_queue_send_receive_teardown,
	},
};

const bench_suite_t bench_suite_native_posix = {
	.name = "native_posix",
	.is_enabled = native_posix_is_enabled,
	.cases = native_posix_cases,
	.case_count = sizeof(native_posix_cases) / sizeof(native_posix_cases[0]),
};

#else /* !CONFIG_OVE_RTOS_POSIX */

static int native_posix_is_enabled(void)
{
	return 0;
}

const bench_suite_t bench_suite_native_posix = {
	.name = "native_posix",
	.is_enabled = native_posix_is_enabled,
	.cases = NULL,
	.case_count = 0,
};

#endif /* CONFIG_OVE_RTOS_POSIX */
