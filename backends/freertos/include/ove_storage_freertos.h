/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_STORAGE_FREERTOS_H
#define OVE_STORAGE_FREERTOS_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "event_groups.h"
#include "stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sync primitives ──────────────────────────────────────────────── */

struct ove_mutex {
	StaticSemaphore_t static_sem;
	SemaphoreHandle_t sem;
};

struct ove_sem {
	StaticSemaphore_t static_sem;
	SemaphoreHandle_t sem;
};

struct ove_event {
	StaticSemaphore_t static_sem;
	SemaphoreHandle_t sem;
};

struct ove_condvar {
	StaticSemaphore_t static_guard;
	SemaphoreHandle_t guard;
	struct condvar_waiter *head;
};

typedef struct ove_mutex   ove_mutex_storage_t;
typedef struct ove_sem     ove_sem_storage_t;
typedef struct ove_event   ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	StaticSemaphore_t static_done_sem;
	SemaphoreHandle_t done_sem;
	TaskHandle_t task;
	StaticTask_t static_task;
	void (*entry)(void *);
	void *arg;
};

typedef struct ove_thread ove_thread_storage_t;

/* ── Queue ────────────────────────────────────────────────────────── */

struct ove_queue {
	StaticQueue_t static_queue;
	QueueHandle_t queue;
	uint8_t *storage;
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	StaticTimer_t static_timer;
	TimerHandle_t handle;
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	StaticEventGroup_t static_eg;
	EventGroupHandle_t handle;
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

#define OVE_WQ_QUEUE_DEPTH 16

struct ove_work {
	void (*handler)(struct ove_work *);
	TimerHandle_t delay_timer;
	StaticTimer_t static_timer;
	struct ove_workqueue *target_wq;
};

struct ove_workqueue {
	TaskHandle_t task;
	StaticTask_t static_task;
	StaticQueue_t static_queue;
	uint8_t queue_storage[OVE_WQ_QUEUE_DEPTH * sizeof(struct ove_work *)];
	QueueHandle_t queue;
	StaticSemaphore_t static_done_sem;
	SemaphoreHandle_t done_sem;
	volatile int running;
};

typedef struct ove_workqueue ove_workqueue_storage_t;
typedef struct ove_work     ove_work_storage_t;

/* ── Stream ───────────────────────────────────────────────────────── */

struct ove_stream {
	StreamBufferHandle_t handle;
	StaticStreamBuffer_t static_stream;
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */
/*
 * The FreeRTOS watchdog backend is board-specific (e.g. STM32 IWDG).
 * Board headers may provide the full struct definition before this header.
 * If not defined, provide a generic stub-compatible layout.
 */
#ifndef OVE_WATCHDOG_DEFINED
struct ove_watchdog {
	uint32_t timeout_ms;
	int started;
};
#endif

typedef struct ove_watchdog ove_watchdog_storage_t;

/* ── Filesystem ───────────────────────────────────────────────────── */
/*
 * The FreeRTOS FS backend is board-specific (e.g. FatFS).
 * Board headers may provide the full struct definitions before this header.
 * If not defined, provide generic stub-compatible layouts.
 */
#ifndef OVE_FS_DEFINED
struct ove_file {
	int fd;
};
struct ove_dir {
	void *dp;
};
#endif

typedef struct ove_file ove_file_storage_t;
typedef struct ove_dir  ove_dir_storage_t;

/* ── ML inference ─────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_INFER
struct ove_model {
	const void *model_data;
	size_t      model_size;
	uint8_t    *arena;
	size_t      arena_size;
	void       *interpreter;
	void       *resolver;
	uint64_t    last_invoke_us;
	int         heap_allocated;
};

typedef struct ove_model ove_model_storage_t;
#endif /* CONFIG_OVE_INFER */

#ifdef __cplusplus
}
#endif

#endif /* OVE_STORAGE_FREERTOS_H */
