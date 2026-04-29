/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Native Zephyr baseline — bypasses oveRTOS entirely so the
 * comparison report can show "<binding> wrapper vs raw Zephyr API:
 * 0 ns delta within 95% CI".  Only meaningful on the Zephyr backend;
 * everywhere else the suite reports as disabled.
 *
 * Cases here MUST mirror operations measured in bench_thread.c,
 * bench_sync.c, bench_queue.c, and bench_stream.c so
 * scripts/bench_compare.py can join them by case stem.  Naming
 * convention: native_<wrapped_case_name>.
 *
 * Zephyr maps cleanly to oveRTOS abstractions:
 *   - k_mutex_*           ↔ ove_mutex_*
 *   - k_sem_*             ↔ ove_sem_*
 *   - k_condvar_*         ↔ ove_condvar_*
 *   - k_event_post/wait   ↔ ove_event_*
 *   - k_msgq_*            ↔ ove_queue_*
 *   - k_pipe_read/write   ↔ ove_stream_*  (byte-stream IPC)
 *   - k_thread_create     ↔ ove_thread_*
 * Recursive mutex: Zephyr's k_mutex IS recursive by default (locking
 * the same mutex twice from the same thread increments a counter), so
 * we use k_mutex for both regular and recursive cases — matching the
 * wrapper's semantics on Zephyr.
 */

#include "benchmark.h"
#include "ove/ove.h"

#if defined(CONFIG_OVE_RTOS_ZEPHYR)

#include <zephyr/kernel.h>
#include <stdint.h>
#include <string.h>

/* ─── Shared state ─────────────────────────────────────────────── */

static struct k_mutex   native_mtx;
static struct k_mutex   native_rmtx;
static struct k_sem     native_sem;
static struct k_condvar native_cv;
static struct k_mutex   native_cv_mtx;
static struct k_event   native_evt;
static struct k_msgq    native_msgq;
static struct k_pipe    native_pipe;

/* Static stacks/storage — kept off-heap to avoid kernel allocations
 * affecting bench timing.  Sizes large enough for the bench thread
 * helpers (contention pong, ctx-switch pong, condvar/event signaller). */
#define NATIVE_THREAD_STACK 1024
K_THREAD_STACK_DEFINE(native_contention_stack, NATIVE_THREAD_STACK);
K_THREAD_STACK_DEFINE(native_cv_signal_stack,  NATIVE_THREAD_STACK);
K_THREAD_STACK_DEFINE(native_evt_signal_stack, NATIVE_THREAD_STACK);
K_THREAD_STACK_DEFINE(native_cs_pong_stack,    NATIVE_THREAD_STACK);
K_THREAD_STACK_DEFINE(native_create_destroy_stack, NATIVE_THREAD_STACK);

static struct k_thread native_contention_th;
static struct k_thread native_cv_signal_th;
static struct k_thread native_evt_signal_th;
static struct k_thread native_cs_pong_th;
static struct k_thread native_create_destroy_th;

/* msgq backing buffer — 4 entries × 8 bytes (matches the wrapper's
 * `int`-sized message in bench_queue.c). */
#define NATIVE_MSGQ_MAXMSGS 4
#define NATIVE_MSGQ_MSGSIZE 8
static char native_msgq_buf[NATIVE_MSGQ_MAXMSGS * NATIVE_MSGQ_MSGSIZE]
	__attribute__((aligned(4)));

/* k_pipe backing buffer + tx/rx buffers (static — see comment in
 * bench_native_freertos.c about avoiding per-iter stack memclr). */
#define NATIVE_PIPE_BUFSIZE 256
static uint8_t native_pipe_buf[NATIVE_PIPE_BUFSIZE];
static uint8_t native_pipe_tx[64];
static uint8_t native_pipe_rx[64];

static volatile int contention_done;
static volatile uint32_t contention_count;

/* ─── Mutex: lock/unlock ───────────────────────────────────────── */

static void native_mutex_lock_unlock_setup(void *ctx)
{
	(void)ctx;
	k_mutex_init(&native_mtx);
}

static void native_mutex_lock_unlock_run(void *ctx)
{
	(void)ctx;
	k_mutex_lock(&native_mtx, K_FOREVER);
	k_mutex_unlock(&native_mtx);
}

static void native_mutex_lock_unlock_teardown(void *ctx)
{
	(void)ctx;
	/* k_mutex has no destroy — it's a static init. */
}

/* ─── Mutex: create/destroy ────────────────────────────────────── */

static void native_mutex_create_destroy_run(void *ctx)
{
	(void)ctx;
	struct k_mutex m;
	k_mutex_init(&m);
	/* No destroy. */
}

/* ─── Mutex: 2-thread contention throughput ─────────────────────── */

static void native_contention_thread(void *p1, void *p2, void *p3)
{
	(void)p1; (void)p2; (void)p3;
	while (!contention_done) {
		k_mutex_lock(&native_mtx, K_FOREVER);
		contention_count++;
		k_mutex_unlock(&native_mtx);
	}
}

static void native_mutex_contention_setup(void *ctx)
{
	(void)ctx;
	contention_done = 0;
	contention_count = 0;
	k_mutex_init(&native_mtx);
	k_thread_create(&native_contention_th, native_contention_stack,
			NATIVE_THREAD_STACK, native_contention_thread,
			NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
}

static void native_mutex_contention_run(void *ctx)
{
	(void)ctx;
	k_mutex_lock(&native_mtx, K_FOREVER);
	contention_count++;
	k_mutex_unlock(&native_mtx);
}

static void native_mutex_contention_teardown(void *ctx)
{
	(void)ctx;
	contention_done = 1;
	k_thread_join(&native_contention_th, K_FOREVER);
}

/* ─── Recursive mutex: lock/unlock ─────────────────────────────── */

static void native_recursive_mutex_lock_unlock_setup(void *ctx)
{
	(void)ctx;
	/* k_mutex is intrinsically recursive — same primitive used. */
	k_mutex_init(&native_rmtx);
}

static void native_recursive_mutex_lock_unlock_run(void *ctx)
{
	(void)ctx;
	/* 1-deep lock/unlock to match wrapper bench geometry — see
	 * comment in bench_native_freertos.c. */
	k_mutex_lock(&native_rmtx, K_FOREVER);
	k_mutex_unlock(&native_rmtx);
}

static void native_recursive_mutex_lock_unlock_teardown(void *ctx)
{
	(void)ctx;
}

/* ─── Semaphore: take/give ─────────────────────────────────────── */

static void native_sem_take_give_setup(void *ctx)
{
	(void)ctx;
	k_sem_init(&native_sem, 1, 1);
}

static void native_sem_take_give_run(void *ctx)
{
	(void)ctx;
	k_sem_take(&native_sem, K_FOREVER);
	k_sem_give(&native_sem);
}

static void native_sem_take_give_teardown(void *ctx)
{
	(void)ctx;
}

/* ─── Semaphore: create/destroy ────────────────────────────────── */

static void native_sem_create_destroy_run(void *ctx)
{
	(void)ctx;
	struct k_sem s;
	k_sem_init(&s, 0, 1);
}

/* ─── Condvar: signal/wait round trip ──────────────────────────── */

static volatile int native_cv_done;

static void native_cv_signal_thread(void *p1, void *p2, void *p3)
{
	(void)p1; (void)p2; (void)p3;
	while (!native_cv_done) {
		k_mutex_lock(&native_cv_mtx, K_FOREVER);
		k_condvar_signal(&native_cv);
		k_mutex_unlock(&native_cv_mtx);
		k_yield();
	}
}

static void native_condvar_signal_wait_setup(void *ctx)
{
	(void)ctx;
	k_mutex_init(&native_cv_mtx);
	k_condvar_init(&native_cv);
	native_cv_done = 0;
	k_thread_create(&native_cv_signal_th, native_cv_signal_stack,
			NATIVE_THREAD_STACK, native_cv_signal_thread,
			NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
}

static void native_condvar_signal_wait_run(void *ctx)
{
	(void)ctx;
	k_mutex_lock(&native_cv_mtx, K_FOREVER);
	k_condvar_wait(&native_cv, &native_cv_mtx, K_MSEC(10));
	k_mutex_unlock(&native_cv_mtx);
}

static void native_condvar_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	native_cv_done = 1;
	k_mutex_lock(&native_cv_mtx, K_FOREVER);
	k_condvar_broadcast(&native_cv);
	k_mutex_unlock(&native_cv_mtx);
	k_thread_join(&native_cv_signal_th, K_FOREVER);
}

/* ─── Event: signal/wait round trip (Zephyr k_event) ──────────── */

#define NATIVE_EVT_BIT 0x1
static volatile int native_evt_done;

static void native_event_signal_thread(void *p1, void *p2, void *p3)
{
	(void)p1; (void)p2; (void)p3;
	while (!native_evt_done) {
		k_event_post(&native_evt, NATIVE_EVT_BIT);
		k_yield();
	}
}

static void native_event_signal_wait_setup(void *ctx)
{
	(void)ctx;
	k_event_init(&native_evt);
	native_evt_done = 0;
	k_thread_create(&native_evt_signal_th, native_evt_signal_stack,
			NATIVE_THREAD_STACK, native_event_signal_thread,
			NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
}

static void native_event_signal_wait_run(void *ctx)
{
	(void)ctx;
	(void)k_event_wait(&native_evt, NATIVE_EVT_BIT,
			   true /* reset */, K_MSEC(10));
}

static void native_event_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	native_evt_done = 1;
	k_thread_join(&native_evt_signal_th, K_FOREVER);
}

/* ─── Thread: yield ────────────────────────────────────────────── */

static void native_thread_yield_run(void *ctx)
{
	(void)ctx;
	k_yield();
}

/* ─── Thread: sleep_1ms ────────────────────────────────────────── */

static void native_thread_sleep_1ms_run(void *ctx)
{
	(void)ctx;
	k_msleep(1);
}

/* ─── Thread: create/destroy ──────────────────────────────────── */

static void native_thread_noop(void *p1, void *p2, void *p3)
{
	(void)p1; (void)p2; (void)p3;
}

static void native_thread_create_destroy_run(void *ctx)
{
	(void)ctx;
	k_thread_create(&native_create_destroy_th, native_create_destroy_stack,
			NATIVE_THREAD_STACK, native_thread_noop,
			NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_join(&native_create_destroy_th, K_FOREVER);
}

/* ─── Thread: context_switch (2-thread ping-pong via 2 sems) ────── */

static struct k_sem native_cs_a, native_cs_b;
static volatile int native_cs_done;

static void native_cs_pong_thread(void *p1, void *p2, void *p3)
{
	(void)p1; (void)p2; (void)p3;
	while (!native_cs_done) {
		k_sem_take(&native_cs_a, K_FOREVER);
		if (native_cs_done)
			break;
		k_sem_give(&native_cs_b);
	}
}

static void native_thread_context_switch_setup(void *ctx)
{
	(void)ctx;
	k_sem_init(&native_cs_a, 0, 1);
	k_sem_init(&native_cs_b, 0, 1);
	native_cs_done = 0;
	k_thread_create(&native_cs_pong_th, native_cs_pong_stack,
			NATIVE_THREAD_STACK, native_cs_pong_thread,
			NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
}

static void native_thread_context_switch_run(void *ctx)
{
	(void)ctx;
	k_sem_give(&native_cs_a);
	k_sem_take(&native_cs_b, K_FOREVER);
}

static void native_thread_context_switch_teardown(void *ctx)
{
	(void)ctx;
	native_cs_done = 1;
	k_sem_give(&native_cs_a); /* unblock pong so it can exit */
	k_thread_join(&native_cs_pong_th, K_FOREVER);
}

/* ─── Queue: send/receive (k_msgq) ─────────────────────────────── */

static void native_queue_send_receive_setup(void *ctx)
{
	(void)ctx;
	k_msgq_init(&native_msgq, native_msgq_buf,
		    NATIVE_MSGQ_MSGSIZE, NATIVE_MSGQ_MAXMSGS);
}

static void native_queue_send_receive_run(void *ctx)
{
	(void)ctx;
	uint8_t buf[NATIVE_MSGQ_MSGSIZE] = { 0 };
	(void)k_msgq_put(&native_msgq, buf, K_FOREVER);
	(void)k_msgq_get(&native_msgq, buf, K_FOREVER);
}

static void native_queue_send_receive_teardown(void *ctx)
{
	(void)ctx;
	k_msgq_cleanup(&native_msgq);
}

/* ─── Queue: create/destroy ────────────────────────────────────── */

static void native_queue_create_destroy_run(void *ctx)
{
	(void)ctx;
	struct k_msgq q;
	static char qbuf[NATIVE_MSGQ_MAXMSGS * NATIVE_MSGQ_MSGSIZE]
		__attribute__((aligned(4)));
	k_msgq_init(&q, qbuf, NATIVE_MSGQ_MSGSIZE, NATIVE_MSGQ_MAXMSGS);
	k_msgq_cleanup(&q);
}

/* ─── Stream: send/recv 64B (k_pipe) ───────────────────────────── */

static void native_stream_send_recv_64B_setup(void *ctx)
{
	(void)ctx;
	k_pipe_init(&native_pipe, native_pipe_buf, NATIVE_PIPE_BUFSIZE);
	memset(native_pipe_tx, 0xAA, sizeof(native_pipe_tx));
}

static void native_stream_send_recv_64B_run(void *ctx)
{
	(void)ctx;
	(void)k_pipe_write(&native_pipe, native_pipe_tx, 64, K_FOREVER);
	(void)k_pipe_read(&native_pipe, native_pipe_rx, 64, K_FOREVER);
}

/* ─── Suite registration ─────────────────────────────────────── */

static int native_zephyr_is_enabled(void)
{
	return 1;
}

static const bench_case_t native_zephyr_cases[] = {
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
	/* IPC: queue (k_msgq) + stream (k_pipe — Zephyr's native byte-
	 * stream primitive, the closest analogue to oveRTOS stream). */
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
		.setup = native_stream_send_recv_64B_setup,
		.run = native_stream_send_recv_64B_run,
	},
};

const bench_suite_t bench_suite_native_zephyr = {
	.name = "native_zephyr",
	.is_enabled = native_zephyr_is_enabled,
	.cases = native_zephyr_cases,
	.case_count = sizeof(native_zephyr_cases) / sizeof(native_zephyr_cases[0]),
};

#else /* !CONFIG_OVE_RTOS_ZEPHYR */

static int native_zephyr_is_enabled(void)
{
	return 0;
}

const bench_suite_t bench_suite_native_zephyr = {
	.name = "native_zephyr",
	.is_enabled = native_zephyr_is_enabled,
	.cases = NULL,
	.case_count = 0,
};

#endif /* CONFIG_OVE_RTOS_ZEPHYR */
