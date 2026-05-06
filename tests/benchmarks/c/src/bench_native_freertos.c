/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Native FreeRTOS baseline — bypasses oveRTOS entirely so the
 * comparison report can show "<binding> wrapper vs raw FreeRTOS API:
 * 0 ns delta within 95% CI".  Only meaningful on the FreeRTOS backend;
 * everywhere else the suite reports as disabled.
 *
 * Cases here MUST mirror operations measured in bench_thread.c,
 * bench_sync.c, bench_queue.c, and bench_stream.c so
 * scripts/bench_compare.py can join them by case stem.
 *
 * Storage strategy: every kernel object is allocated via the
 * `*Static` variants (xSemaphoreCreateMutexStatic, xQueueCreateStatic,
 * xTaskCreateStatic, ...) backed by file-scope `Static*_t` storage.
 * This keeps the file compatible with FreeRTOS zero-heap builds (where
 * `configSUPPORT_DYNAMIC_ALLOCATION=0` removes the dynamic variants
 * entirely) and makes setup paths heap-free in both modes.  The
 * `*_create_destroy_run` cases (heap-mode-only by definition) are
 * gated under `#ifndef CONFIG_OVE_ZERO_HEAP`.
 *
 * No `eventgroup` / `workqueue` cases here:
 *   - eventgroups: FreeRTOS DOES have xEventGroup* but oveRTOS event
 *     uses xTaskNotify (lighter) — comparing those side-by-side would
 *     be apples-to-orangutans.
 *   - workqueues: FreeRTOS has no native equivalent.
 */

#include "benchmark.h"
#include "ove/ove.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS)

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "stream_buffer.h"
#include <string.h>

/* ─── Shared storage (Static* — works in both heap and zero-heap modes) ─ */

/* Sized for the largest stack any helper needs (contention/cv/evt/cs
 * pong-style helpers run lock+inc+unlock or notify-take loops that
 * fit comfortably in 512 bytes). */
#define NATIVE_TASK_STACK 512

static StaticSemaphore_t native_mtx_storage;
static StaticSemaphore_t native_rmtx_storage;
static StaticSemaphore_t native_sem_storage;
static StaticSemaphore_t native_cs_a_storage;
static StaticSemaphore_t native_cs_b_storage;
static StaticQueue_t native_queue_storage;
static uint8_t native_queue_buf[8 * sizeof(uint32_t)];
static StaticStreamBuffer_t native_stream_storage;
static uint8_t native_stream_buf[256];

static StaticTask_t native_contention_tcb;
static StackType_t native_contention_stack[NATIVE_TASK_STACK];
static StaticTask_t native_cv_tcb;
static StackType_t native_cv_stack[NATIVE_TASK_STACK];
static StaticTask_t native_evt_tcb;
static StackType_t native_evt_stack[NATIVE_TASK_STACK];
static StaticTask_t native_cs_tcb;
static StackType_t native_cs_stack[NATIVE_TASK_STACK];

static SemaphoreHandle_t native_mtx;
static SemaphoreHandle_t native_rmtx;
static SemaphoreHandle_t native_sem;
static SemaphoreHandle_t native_cs_a;
static SemaphoreHandle_t native_cs_b;
static QueueHandle_t native_queue;
static StreamBufferHandle_t native_stream;

/* ─── Mutex: lock/unlock ───────────────────────────────────────── */

static void native_mutex_lock_unlock_setup(void *ctx)
{
	(void)ctx;
	native_mtx = xSemaphoreCreateMutexStatic(&native_mtx_storage);
}

static void native_mutex_lock_unlock_run(void *ctx)
{
	(void)ctx;
	xSemaphoreTake(native_mtx, portMAX_DELAY);
	xSemaphoreGive(native_mtx);
}

static void native_mutex_lock_unlock_teardown(void *ctx)
{
	(void)ctx;
	/* No vSemaphoreDelete — undefined for static-allocated objects in
	 * zero-heap builds; storage is reusable for the next setup. */
}

/* ─── Mutex: create/destroy (heap-mode only) ───────────────────── */
#ifndef CONFIG_OVE_ZERO_HEAP
static void native_mutex_create_destroy_run(void *ctx)
{
	(void)ctx;
	SemaphoreHandle_t m = xSemaphoreCreateMutex();
	vSemaphoreDelete(m);
}
#endif

/* ─── Mutex: 2-thread contention ────────────────────────────────── */

static volatile int contention_done;
static volatile uint32_t contention_count;

static void native_contention_task(void *arg)
{
	(void)arg;
	while (!contention_done) {
		xSemaphoreTake(native_mtx, portMAX_DELAY);
		contention_count++;
		xSemaphoreGive(native_mtx);
	}
	vTaskDelete(NULL);
}

static void native_mutex_contention_setup(void *ctx)
{
	(void)ctx;
	contention_done = 0;
	contention_count = 0;
	native_mtx = xSemaphoreCreateMutexStatic(&native_mtx_storage);
	(void)xTaskCreateStatic(native_contention_task, "nat_cont", NATIVE_TASK_STACK, NULL,
				tskIDLE_PRIORITY + 1, native_contention_stack,
				&native_contention_tcb);
}

static void native_mutex_contention_run(void *ctx)
{
	(void)ctx;
	xSemaphoreTake(native_mtx, portMAX_DELAY);
	contention_count++;
	xSemaphoreGive(native_mtx);
}

static void native_mutex_contention_teardown(void *ctx)
{
	(void)ctx;
	contention_done = 1;
	/* Let the contention task observe the flag and self-delete. */
	vTaskDelay(pdMS_TO_TICKS(2));
}

/* ─── Recursive mutex: lock/unlock ─────────────────────────────── */

static void native_recursive_mutex_lock_unlock_setup(void *ctx)
{
	(void)ctx;
	native_rmtx = xSemaphoreCreateRecursiveMutexStatic(&native_rmtx_storage);
}

static void native_recursive_mutex_lock_unlock_run(void *ctx)
{
	(void)ctx;
	/* 1-deep take/give to match wrapper bench geometry — wrapper's
	 * `rmtx_lock_unlock_run` measures `ove_recursive_mutex_lock` +
	 * `ove_recursive_mutex_unlock`, a single full lock/unlock pair on
	 * a recursive-mutex type.  Earlier 2-deep variant pulled in two
	 * cheap counter-increment recursive ops (~+200 ns) that made the
	 * native column appear slower than the wrapper. */
	xSemaphoreTakeRecursive(native_rmtx, portMAX_DELAY);
	xSemaphoreGiveRecursive(native_rmtx);
}

static void native_recursive_mutex_lock_unlock_teardown(void *ctx)
{
	(void)ctx;
}

/* ─── Semaphore: take/give ─────────────────────────────────────── */

static void native_sem_take_give_setup(void *ctx)
{
	(void)ctx;
	/* Counting sem with initial count 1 to mirror POSIX `sem_init(...,1)`. */
	native_sem = xSemaphoreCreateCountingStatic(1, 1, &native_sem_storage);
}

static void native_sem_take_give_run(void *ctx)
{
	(void)ctx;
	xSemaphoreTake(native_sem, portMAX_DELAY);
	xSemaphoreGive(native_sem);
}

static void native_sem_take_give_teardown(void *ctx)
{
	(void)ctx;
}

/* ─── Semaphore: create/destroy (heap-mode only) ───────────────── */
#ifndef CONFIG_OVE_ZERO_HEAP
static void native_sem_create_destroy_run(void *ctx)
{
	(void)ctx;
	SemaphoreHandle_t s = xSemaphoreCreateBinary();
	vSemaphoreDelete(s);
}
#endif

/* ─── Condvar/event: signal/wait via task notification ──────────
 *
 * FreeRTOS lacks a native condvar primitive; xTaskNotify is the
 * canonical lightweight signaling mechanism and is what oveRTOS
 * condvar/event ultimately wraps on FreeRTOS.  Same machinery is
 * used for both `condvar_signal_wait` and `event_signal_wait`
 * baselines so the report can compare oveRTOS condvar vs oveRTOS
 * event against the same underlying FreeRTOS primitive.
 */

static TaskHandle_t native_cv_waiter;
static TaskHandle_t native_cv_signaller;
static volatile int native_cv_done;

static void native_cv_signal_task(void *arg)
{
	(void)arg;
	while (!native_cv_done) {
		xTaskNotifyGive(native_cv_waiter);
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	}
	vTaskDelete(NULL);
}

static void native_condvar_signal_wait_setup(void *ctx)
{
	(void)ctx;
	native_cv_done = 0;
	native_cv_waiter = xTaskGetCurrentTaskHandle();
	native_cv_signaller = xTaskCreateStatic(native_cv_signal_task, "nat_cv", NATIVE_TASK_STACK,
						NULL, tskIDLE_PRIORITY + 1, native_cv_stack,
						&native_cv_tcb);
}

static void native_condvar_signal_wait_run(void *ctx)
{
	(void)ctx;
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	xTaskNotifyGive(native_cv_signaller);
}

static void native_condvar_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	native_cv_done = 1;
	xTaskNotifyGive(native_cv_signaller);
	vTaskDelay(pdMS_TO_TICKS(2));
}

/* ─── Event: signal/wait — second copy of the cv pattern ───────── */

static TaskHandle_t native_evt_waiter;
static TaskHandle_t native_evt_signaller;
static volatile int native_evt_done;

static void native_evt_signal_task(void *arg)
{
	(void)arg;
	while (!native_evt_done) {
		xTaskNotifyGive(native_evt_waiter);
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	}
	vTaskDelete(NULL);
}

static void native_event_signal_wait_setup(void *ctx)
{
	(void)ctx;
	native_evt_done = 0;
	native_evt_waiter = xTaskGetCurrentTaskHandle();
	native_evt_signaller = xTaskCreateStatic(native_evt_signal_task, "nat_evt",
						 NATIVE_TASK_STACK, NULL, tskIDLE_PRIORITY + 1,
						 native_evt_stack, &native_evt_tcb);
}

static void native_event_signal_wait_run(void *ctx)
{
	(void)ctx;
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	xTaskNotifyGive(native_evt_signaller);
}

static void native_event_signal_wait_teardown(void *ctx)
{
	(void)ctx;
	native_evt_done = 1;
	xTaskNotifyGive(native_evt_signaller);
	vTaskDelay(pdMS_TO_TICKS(2));
}

/* ─── Thread: yield ────────────────────────────────────────────── */

static void native_thread_yield_run(void *ctx)
{
	(void)ctx;
	taskYIELD();
}

/* ─── Thread: sleep_1ms ────────────────────────────────────────── */

static void native_thread_sleep_1ms_run(void *ctx)
{
	(void)ctx;
	vTaskDelay(pdMS_TO_TICKS(1));
}

/* ─── Thread: create/destroy (heap-mode only) ─────────────────── */
#ifndef CONFIG_OVE_ZERO_HEAP
static void native_dummy_task(void *arg)
{
	(void)arg;
	/* Block forever; parent will delete us externally. */
	while (1)
		vTaskDelay(portMAX_DELAY);
}

static void native_thread_create_destroy_run(void *ctx)
{
	(void)ctx;
	TaskHandle_t h;
	xTaskCreate(native_dummy_task, "nat_tmp", 256, NULL, tskIDLE_PRIORITY, &h);
	vTaskDelete(h);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ─── Thread: context_switch (2-thread ping-pong via semaphores) ─ */

static volatile int native_cs_done;

static void native_cs_pong_task(void *arg)
{
	(void)arg;
	while (!native_cs_done) {
		xSemaphoreTake(native_cs_a, portMAX_DELAY);
		if (native_cs_done)
			break;
		xSemaphoreGive(native_cs_b);
	}
	vTaskDelete(NULL);
}

static void native_thread_context_switch_setup(void *ctx)
{
	(void)ctx;
	native_cs_a = xSemaphoreCreateBinaryStatic(&native_cs_a_storage);
	native_cs_b = xSemaphoreCreateBinaryStatic(&native_cs_b_storage);
	native_cs_done = 0;
	(void)xTaskCreateStatic(native_cs_pong_task, "nat_cs", NATIVE_TASK_STACK, NULL,
				tskIDLE_PRIORITY + 1, native_cs_stack, &native_cs_tcb);
}

static void native_thread_context_switch_run(void *ctx)
{
	(void)ctx;
	xSemaphoreGive(native_cs_a);
	xSemaphoreTake(native_cs_b, portMAX_DELAY);
}

static void native_thread_context_switch_teardown(void *ctx)
{
	(void)ctx;
	native_cs_done = 1;
	xSemaphoreGive(native_cs_a);
	vTaskDelay(pdMS_TO_TICKS(2));
}

/* ─── IPC queue: send/receive (single 32-bit message) ──────────── */

static void native_queue_send_receive_setup(void *ctx)
{
	(void)ctx;
	native_queue =
		xQueueCreateStatic(8, sizeof(uint32_t), native_queue_buf, &native_queue_storage);
}

static void native_queue_send_receive_run(void *ctx)
{
	(void)ctx;
	uint32_t v = 0xCAFEBABE;
	xQueueSend(native_queue, &v, portMAX_DELAY);
	xQueueReceive(native_queue, &v, portMAX_DELAY);
}

static void native_queue_send_receive_teardown(void *ctx)
{
	(void)ctx;
}

/* ─── IPC queue: create/destroy (heap-mode only) ────────────── */
#ifndef CONFIG_OVE_ZERO_HEAP
static void native_queue_create_destroy_run(void *ctx)
{
	(void)ctx;
	QueueHandle_t q = xQueueCreate(8, sizeof(uint32_t));
	vQueueDelete(q);
}
#endif

/* ─── IPC stream: send/recv 64B via stream buffer ──────────────── */

/* Static tx/rx buffers in BSS — matches wrapper bench geometry
 * (`bench_stream.c::tx_buf` / `rx_buf`).  An on-stack `uint8_t buf[64]
 * = {0};` here would compile to a per-iteration `memclr` of the
 * 64-byte frame (~50 cycles) that the wrapper bench doesn't pay,
 * making the native column appear ~400-600 ns slower than the wrapper
 * — a bench-geometry artifact, not a real cost. */
static uint8_t native_stream_tx[64];
static uint8_t native_stream_rx[64];

static void native_stream_send_recv_64B_setup(void *ctx)
{
	(void)ctx;
	/* 256-byte buffer, trigger level 1 — same shape as oveRTOS bench. */
	native_stream = xStreamBufferCreateStatic(sizeof(native_stream_buf), 1, native_stream_buf,
						  &native_stream_storage);
	memset(native_stream_tx, 0xAA, sizeof(native_stream_tx));
}

static void native_stream_send_recv_64B_run(void *ctx)
{
	(void)ctx;
	xStreamBufferSend(native_stream, native_stream_tx, 64, portMAX_DELAY);
	xStreamBufferReceive(native_stream, native_stream_rx, 64, portMAX_DELAY);
}

static void native_stream_send_recv_64B_teardown(void *ctx)
{
	(void)ctx;
}

/* ─── Suite registration ─────────────────────────────────────── */

static int native_freertos_is_enabled(void)
{
	return 1;
}

static const bench_case_t native_freertos_cases[] = {
	/* Mutex */
	{
		.name = "native_mutex_lock_unlock",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_mutex_lock_unlock_setup,
		.run = native_mutex_lock_unlock_run,
		.teardown = native_mutex_lock_unlock_teardown,
	},
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "native_mutex_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = native_mutex_create_destroy_run,
	},
#endif
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
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "native_sem_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = native_sem_create_destroy_run,
	},
#endif
	/* Condvar / event (xTaskNotify-based) */
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
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "native_thread_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = native_thread_create_destroy_run,
		.iterations = 200,
	},
#endif
	{
		.name = "native_thread_context_switch",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_thread_context_switch_setup,
		.run = native_thread_context_switch_run,
		.teardown = native_thread_context_switch_teardown,
		.iterations = 500,
	},
	/* IPC (queue + stream buffer) */
	{
		.name = "native_queue_send_receive",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_queue_send_receive_setup,
		.run = native_queue_send_receive_run,
		.teardown = native_queue_send_receive_teardown,
	},
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "native_queue_create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = native_queue_create_destroy_run,
	},
#endif
	{
		.name = "native_stream_send_recv_64B",
		.type = BENCH_TYPE_LATENCY,
		.setup = native_stream_send_recv_64B_setup,
		.run = native_stream_send_recv_64B_run,
		.teardown = native_stream_send_recv_64B_teardown,
	},
};

const bench_suite_t bench_suite_native_freertos = {
	.name = "native_freertos",
	.is_enabled = native_freertos_is_enabled,
	.cases = native_freertos_cases,
	.case_count = sizeof(native_freertos_cases) / sizeof(native_freertos_cases[0]),
};

#else /* !CONFIG_OVE_RTOS_FREERTOS */

static int native_freertos_is_enabled(void)
{
	return 0;
}

const bench_suite_t bench_suite_native_freertos = {
	.name = "native_freertos",
	.is_enabled = native_freertos_is_enabled,
	.cases = NULL,
	.case_count = 0,
};

#endif /* CONFIG_OVE_RTOS_FREERTOS */
