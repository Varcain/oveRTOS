/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Native NuttX baseline — bypasses oveRTOS entirely so the comparison
 * report can show "<binding> wrapper vs raw NuttX API: 0 ns delta within
 * 95% CI".  Only meaningful on the NuttX backend; everywhere else the
 * suite reports as disabled.
 *
 * Cases here MUST mirror operations measured in bench_thread.c,
 * bench_sync.c, bench_queue.c, and bench_stream.c so
 * scripts/bench_compare.py can join them by case stem.  Naming
 * convention: native_<wrapped_case_name>.  Cases without a meaningful
 * raw-NuttX equivalent (event groups, workqueues, byte-stream IPC —
 * none of which are kernel primitives on NuttX) are intentionally
 * absent and noted in the report.
 *
 * Native NuttX is POSIX: pthread_mutex / pthread_cond / sem / pthread.
 * The one non-POSIX primitive used is `mq_*` for the queue baseline
 * (NuttX's canonical kernel-side message queue, semantically closer
 * to the wrapper's `ove_queue` than POSIX pipes).
 */

#include "benchmark.h"
#include "ove/ove.h"

#if defined(CONFIG_OVE_RTOS_NUTTX)

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* NuttX kernel task API — used by `native_thread_create_destroy` to
 * match the wrapper's semantics (the wrapper's `ove_thread_create_`
 * uses `task_create` for independent task lifetime, mirroring
 * FreeRTOS `xTaskCreateStatic` semantics).  Comparing against
 * pthread_create here would understate wrapper overhead since pthread
 * shares the parent's task_group_s and skips ~140 µs of group_alloc
 * that task_create pays per call.  See `backends/nuttx/nuttx_thread.c`
 * comment around `task_create()` for why the wrapper picked that
 * primitive. */
#include <nuttx/sched.h>

/* ─── Shared state ─────────────────────────────────────────────── */

static pthread_mutex_t native_mtx;
static pthread_mutex_t native_rmtx;
static sem_t native_sem;
static pthread_cond_t native_cv;
static pthread_mutex_t native_cv_mtx;
static bool native_event_flag;
static mqd_t native_mq = (mqd_t)-1;

#define NATIVE_MQ_NAME "/ove_bench_nuttx"
/* 8-byte payload for queue send/receive (matches wrapper's `int`-sized
 * message in `bench_queue.c`).  NuttX `CONFIG_MQ_MAXMSGSIZE` must be
 * >= this; default of 32 is fine. */
#define NATIVE_MQ_MSGSIZE 8
#define NATIVE_MQ_MAXMSGS 4

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
	/* 1-deep lock/unlock to match wrapper bench geometry — see
	 * comment in bench_native_freertos.c. */
	pthread_mutex_lock(&native_rmtx);
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
 * NuttX has no native "event" primitive — same situation as POSIX.
 * The canonical equivalent is a bool guarded by pthread_mutex +
 * pthread_cond.  Same machinery as condvar_signal_wait above; we
 * keep both rows so the report can show how oveRTOS event vs
 * oveRTOS condvar compares against the same underlying NuttX/POSIX
 * primitive.
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

/* The wrapper's `ove_thread_create_` uses NuttX `task_create()` (full
 * independent-task semantics matching FreeRTOS xTaskCreateStatic), not
 * `pthread_create()`.  `task_create` runs `group_allocate()` per call
 * (~140 µs on STM32F7) which pthread skips by sharing the parent's
 * task_group_s.  Match the wrapper's primitive here so the wrapper-vs-
 * native delta reflects pure binding overhead rather than the
 * task-vs-pthread cost asymmetry.
 *
 * `task_create` takes argv-style entry; we encode the noop entry as
 * argv[0] (the task name) — `argv[1] == NULL` so the entry function
 * sees "no args" and returns immediately. */

static int native_task_noop(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	return 0;
}

static void native_thread_create_destroy_run(void *ctx)
{
	(void)ctx;
	pid_t pid = task_create("ove_bench_noop", SCHED_PRIORITY_DEFAULT,
				1024, native_task_noop, NULL);
	if (pid > 0) {
		/* Wait for the noop task to finish — `waitpid` is the
		 * canonical NuttX-side join for `task_create`.  pthread_join
		 * doesn't apply since this is a task, not a pthread. */
		int status;
		(void)waitpid(pid, &status, 0);
	}
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
	sem_post(&native_cs_a);
	pthread_join(native_cs_thread, NULL);
	sem_destroy(&native_cs_a);
	sem_destroy(&native_cs_b);
}

/* ─── Queue: send/receive (NuttX message queue) ────────────────── */

static void native_queue_send_receive_setup(void *ctx)
{
	(void)ctx;
	struct mq_attr attr = {
		.mq_maxmsg = NATIVE_MQ_MAXMSGS,
		.mq_msgsize = NATIVE_MQ_MSGSIZE,
	};
	mq_unlink(NATIVE_MQ_NAME); /* clear any leftover from a prior run */
	native_mq = mq_open(NATIVE_MQ_NAME, O_CREAT | O_RDWR, 0666, &attr);
}

static void native_queue_send_receive_run(void *ctx)
{
	(void)ctx;
	uint8_t buf[NATIVE_MQ_MSGSIZE] = { 0 };
	mq_send(native_mq, (const char *)buf, NATIVE_MQ_MSGSIZE, 0);
	mq_receive(native_mq, (char *)buf, NATIVE_MQ_MSGSIZE, NULL);
}

static void native_queue_send_receive_teardown(void *ctx)
{
	(void)ctx;
	if (native_mq != (mqd_t)-1) {
		mq_close(native_mq);
		mq_unlink(NATIVE_MQ_NAME);
		native_mq = (mqd_t)-1;
	}
}

/* ─── Queue: create/destroy ─────────────────────────────────────── */

static void native_queue_create_destroy_run(void *ctx)
{
	(void)ctx;
	struct mq_attr attr = {
		.mq_maxmsg = NATIVE_MQ_MAXMSGS,
		.mq_msgsize = NATIVE_MQ_MSGSIZE,
	};
	mq_unlink(NATIVE_MQ_NAME);
	mqd_t q = mq_open(NATIVE_MQ_NAME, O_CREAT | O_RDWR, 0666, &attr);
	if (q != (mqd_t)-1) {
		mq_close(q);
		mq_unlink(NATIVE_MQ_NAME);
	}
}

/* ─── Suite registration ─────────────────────────────────────── */

static int native_nuttx_is_enabled(void)
{
	return 1;
}

static const bench_case_t native_nuttx_cases[] = {
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
	/* Condvar / event (both backed by pthread_mutex+pthread_cond on NuttX) */
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
	/* IPC: queue (NuttX mq_*).  No native byte-stream primitive on
	 * NuttX (CONFIG_PIPES is off in the bench defconfig); the
	 * `stream/*` rows in the wrapper bench have no native peer and
	 * are intentionally absent — same situation as `eventgroup` /
	 * `workqueue` for FreeRTOS. */
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
};

const bench_suite_t bench_suite_native_nuttx = {
	.name = "native_nuttx",
	.is_enabled = native_nuttx_is_enabled,
	.cases = native_nuttx_cases,
	.case_count = sizeof(native_nuttx_cases) / sizeof(native_nuttx_cases[0]),
};

#else /* !CONFIG_OVE_RTOS_NUTTX */

static int native_nuttx_is_enabled(void)
{
	return 0;
}

const bench_suite_t bench_suite_native_nuttx = {
	.name = "native_nuttx",
	.is_enabled = native_nuttx_is_enabled,
	.cases = NULL,
	.case_count = 0,
};

#endif /* CONFIG_OVE_RTOS_NUTTX */
