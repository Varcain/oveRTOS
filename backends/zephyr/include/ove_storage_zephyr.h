/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_STORAGE_ZEPHYR_H
#define OVE_STORAGE_ZEPHYR_H

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sync primitives ──────────────────────────────────────────────── */

struct ove_mutex {
	struct k_mutex mtx;
};

struct ove_sem {
	struct k_sem sem;
};

struct ove_event {
	struct k_sem sem;
};

struct ove_condvar {
	struct k_condvar cv;
};

typedef struct ove_mutex   ove_mutex_storage_t;
typedef struct ove_sem     ove_sem_storage_t;
typedef struct ove_event   ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	struct k_thread thread;
	k_thread_stack_t *stack;
	size_t stack_size;
	int heap_stack;
};

typedef struct ove_thread ove_thread_storage_t;

/* ── Queue ────────────────────────────────────────────────────────── */

struct ove_queue {
	struct k_msgq msgq;
	char *buffer;
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	struct k_timer timer;
	struct k_work work;
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
	uint32_t period_ms;
	int one_shot;
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	struct k_event event;
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

struct ove_work {
	struct k_work_delayable dwork;
	void (*handler)(struct ove_work *);
};

struct ove_workqueue {
	struct k_work_q work_q;
	k_thread_stack_t *stack;
	size_t stack_size;
};

typedef struct ove_workqueue ove_workqueue_storage_t;
typedef struct ove_work     ove_work_storage_t;

/* ── Stream ───────────────────────────────────────────────────────── */

struct ove_stream {
	struct k_pipe pipe;
	unsigned char *buffer;
	size_t size;
	atomic_t bytes_count;
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */

struct ove_watchdog {
	const struct device *dev;
	int channel_id;
	uint32_t timeout_ms;
	int started;
};

typedef struct ove_watchdog ove_watchdog_storage_t;

/* ── Filesystem ───────────────────────────────────────────────────── */

struct ove_file {
	struct fs_file_t file;
};

struct ove_dir {
	struct fs_dir_t dir;
};

typedef struct ove_file ove_file_storage_t;
typedef struct ove_dir  ove_dir_storage_t;

/*
 * Zephyr thread stacks require K_THREAD_STACK_DEFINE for MPU alignment.
 * Override the generic OVE_THREAD_DEFINE / OVE_THREAD_DEFINE_STATIC
 * stack allocation to use the Zephyr macro.
 */
#define OVE_THREAD_STACK_DEFINE_(name, size) \
	K_THREAD_STACK_DEFINE(name, size)

/* On Zephyr, block-scope stacks can't use K_THREAD_STACK_DEFINE (needs
 * file scope for __stackmem section).  Set to NULL so ove_thread_init()
 * allocates a proper stack via k_thread_stack_alloc(). */
#define OVE_THREAD_STACK_BLOCK_STATIC_(name, size) \
	static uint8_t *name = NULL

#define OVE_THREAD_STACK_MEMBER_(name, size) \
	K_KERNEL_STACK_MEMBER(name, size)

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

#endif /* OVE_STORAGE_ZEPHYR_H */
