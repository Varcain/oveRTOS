/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_STORAGE_POSIX_H
#define OVE_STORAGE_POSIX_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sync primitives ──────────────────────────────────────────────── */

struct ove_mutex {
	pthread_mutex_t mtx;
};

struct ove_sem {
	sem_t sem;
};

struct ove_event {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int signaled;
};

struct ove_condvar {
	pthread_cond_t cond;
};

typedef struct ove_mutex   ove_mutex_storage_t;
typedef struct ove_sem     ove_sem_storage_t;
typedef struct ove_event   ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	pthread_t tid;
	void (*entry)(void *arg);       /* ove_thread_fn */
	void *arg;
	int state;                      /* ove_thread_state_t */
	sem_t suspend_sem;
	int started;
};

typedef struct ove_thread ove_thread_storage_t;

/* ── Queue ────────────────────────────────────────────────────────── */

struct ove_queue {
	void *buffer;
	size_t item_size;
	unsigned int max_items;
	unsigned int count;
	unsigned int head;
	unsigned int tail;
	pthread_mutex_t lock;
	pthread_cond_t not_full;
	pthread_cond_t not_empty;
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	timer_t posix_timer;
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
	uint32_t period_ms;
	int one_shot;
	int created;
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t bits;                  /* ove_eventbits_t */
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

#define OVE_WQ_MAX_PENDING 64

struct ove_work {
	void (*handler)(struct ove_work *);
	uint32_t delay_ms;
	int pending;
};

struct ove_workqueue {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct ove_work *queue[OVE_WQ_MAX_PENDING];
	int count;
	int running;
};

typedef struct ove_workqueue ove_workqueue_storage_t;
typedef struct ove_work     ove_work_storage_t;

/* ── Stream ───────────────────────────────────────────────────────── */

struct ove_stream {
	uint8_t *buffer;
	size_t size;
	size_t trigger;
	size_t head;
	size_t tail;
	size_t count;
	pthread_mutex_t lock;
	pthread_cond_t data_avail;
	pthread_cond_t space_avail;
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */

struct ove_watchdog {
	uint32_t timeout_ms;
	int started;
};

typedef struct ove_watchdog ove_watchdog_storage_t;

/* ── Filesystem ───────────────────────────────────────────────────── */

struct ove_file {
	int fd;
};

struct ove_dir {
	DIR *dp;
};

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

#endif /* OVE_STORAGE_POSIX_H */
