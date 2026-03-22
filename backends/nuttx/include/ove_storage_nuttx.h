/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_STORAGE_NUTTX_H
#define OVE_STORAGE_NUTTX_H

#include <stdint.h>
#include <stddef.h>
#include <nuttx/config.h>
#include <pthread.h>
#include <semaphore.h>
#include <nuttx/mutex.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sync primitives ──────────────────────────────────────────────── */

struct ove_mutex {
	union {
		mutex_t mtx;
		rmutex_t rmtx;
	};
};

struct ove_sem {
	sem_t sem;
};

struct ove_event {
	sem_t sem;
};

struct ove_condvar {
	sem_t waiter;
	mutex_t guard;
	int nwaiters;
};

typedef struct ove_mutex   ove_mutex_storage_t;
typedef struct ove_sem     ove_sem_storage_t;
typedef struct ove_event   ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	pid_t pid;
	void (*entry)(void *arg);
	void *arg;
	int state;
	sem_t suspend_sem;
	sem_t done_sem;
	int suspend_inited;
	int started;
};

typedef struct ove_thread ove_thread_storage_t;

/* ── Queue ────────────────────────────────────────────────────────── */

struct ove_queue {
	void *buffer;
	size_t item_size;
	unsigned int max_items;
	unsigned int head;
	unsigned int tail;
	sem_t not_full;
	sem_t not_empty;
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	timer_t posix_timer;
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
	uint32_t period_ms;
	int one_shot;
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	sem_t waiter;
	ove_eventbits_t bits;
	int nwaiters;
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

#define OVE_WQ_QUEUE_DEPTH 16

struct ove_work {
	void (*handler)(struct ove_work *);
	volatile int cancelled;
	uint32_t delay_ms;
	int pending;
};

struct ove_workqueue {
	pid_t worker_pid;
	struct ove_work *ring[OVE_WQ_QUEUE_DEPTH];
	unsigned int head;
	unsigned int tail;
	mutex_t lock;
	sem_t not_full;
	sem_t not_empty;
	sem_t delay_sem;
	volatile int running;
};

typedef struct ove_workqueue ove_workqueue_storage_t;
typedef struct ove_work     ove_work_storage_t;

/* ── Stream ───────────────────────────────────────────────────────── */

struct ove_stream {
	unsigned char *buffer;
	size_t size;
	size_t head;
	size_t tail;
	size_t count;
	pthread_mutex_t lock;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */

struct ove_watchdog {
	int fd;
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

#ifdef __cplusplus
}
#endif

#endif /* OVE_STORAGE_NUTTX_H */
