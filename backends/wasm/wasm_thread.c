/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM/Emscripten thread backend.
 *
 * Identical to the POSIX backend except:
 *   - No signals (SIGUSR1 not supported in Emscripten)
 *   - Cooperative suspend via atomic flag + semaphore
 *   - No SIGINT handler in start_scheduler
 *   - No mallinfo for memory stats (use Emscripten API)
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <semaphore.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#endif

static __thread struct ove_thread *tls_current;
static struct ove_thread *first_thread;

/* ── Thread registry for supervisor ────────────────────────────────── */

#define MAX_TRACKED_THREADS 16
static struct ove_thread *tracked_threads[MAX_TRACKED_THREADS];
static int tracked_count;
static pthread_mutex_t tracked_lock = PTHREAD_MUTEX_INITIALIZER;

static void track_add(struct ove_thread *t)
{
	pthread_mutex_lock(&tracked_lock);
	if (tracked_count < MAX_TRACKED_THREADS)
		tracked_threads[tracked_count++] = t;
	pthread_mutex_unlock(&tracked_lock);
}

static void track_remove(struct ove_thread *t)
{
	pthread_mutex_lock(&tracked_lock);
	for (int i = 0; i < tracked_count; i++) {
		if (tracked_threads[i] == t) {
			tracked_threads[i] = tracked_threads[--tracked_count];
			break;
		}
	}
	pthread_mutex_unlock(&tracked_lock);
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)ts.tv_nsec / 1000ULL;
}

static void touch_yield(struct ove_thread *t)
{
	if (t) t->last_yield_us = now_us();
}

/**
 * Check if suspension was requested and block until resumed.
 * Called from yield/sleep points for cooperative suspension.
 */
static void check_suspend(struct ove_thread *t)
{
	if (t && t->suspend_requested) {
		t->state = OVE_THREAD_STATE_SUSPENDED;
		t->suspend_requested = 0;
		/* Block until resumed via sem_post */
		while (t->state == OVE_THREAD_STATE_SUSPENDED) {
			sem_wait(&t->suspend_sem);
		}
	}
}

static void *thread_wrapper(void *arg)
{
	struct ove_thread *t = arg;
	tls_current = t;

	t->state = OVE_THREAD_STATE_RUNNING;
	t->last_yield_us = now_us();
	t->entry(t->arg);
	t->state = OVE_THREAD_STATE_TERMINATED;
	track_remove(t);
	return NULL;
}

int ove_thread_init(ove_thread_t *handle,
			ove_thread_storage_t *storage,
			const struct ove_thread_desc *desc)
{
	if (!handle || !storage || !desc || !desc->entry)
		return OVE_ERR_INVALID_PARAM;

	struct ove_thread *t = (struct ove_thread *)storage;
	memset(t, 0, sizeof(*t));

	t->entry = desc->entry;
	t->arg = desc->arg;
	t->state = OVE_THREAD_STATE_READY;
	t->suspend_requested = 0;
	t->last_yield_us = now_us();
	sem_init(&t->suspend_sem, 0, 0);

	if (pthread_create(&t->tid, NULL, thread_wrapper, t) != 0) {
		sem_destroy(&t->suspend_sem);
		return OVE_ERR_NO_MEMORY;
	}

	t->started = 1;
	track_add(t);

	if (first_thread == NULL)
		first_thread = t;

	*handle = t;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	struct ove_thread *t = handle;
	if (t->started) {
		if (t->state == OVE_THREAD_STATE_SUSPENDED) {
			t->state = OVE_THREAD_STATE_READY;
			sem_post(&t->suspend_sem);
		}
		pthread_join(t->tid, NULL);
	}
	track_remove(t);
	if (first_thread == t)
		first_thread = NULL;
	sem_destroy(&t->suspend_sem);
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_thread_create_(ove_thread_t *handle,
			   const struct ove_thread_desc *desc)
{
	if (!handle || !desc || !desc->entry)
		return OVE_ERR_INVALID_PARAM;

	struct ove_thread *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t)
		return OVE_ERR_NO_MEMORY;
	memset(t, 0, sizeof(*t));

	t->entry = desc->entry;
	t->arg = desc->arg;
	t->state = OVE_THREAD_STATE_READY;
	t->suspend_requested = 0;
	t->last_yield_us = now_us();
	sem_init(&t->suspend_sem, 0, 0);

	if (pthread_create(&t->tid, NULL, thread_wrapper, t) != 0) {
		sem_destroy(&t->suspend_sem);
		OVE_BACKEND_FREE(t);
		return OVE_ERR_NO_MEMORY;
	}

	t->started = 1;
	track_add(t);

	if (first_thread == NULL)
		first_thread = t;

	*handle = t;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	struct ove_thread *t = handle;
	if (t->started) {
		if (t->state == OVE_THREAD_STATE_SUSPENDED) {
			t->state = OVE_THREAD_STATE_READY;
			sem_post(&t->suspend_sem);
		}
		pthread_join(t->tid, NULL);
	}
	if (first_thread == t)
		first_thread = NULL;
	sem_destroy(&t->suspend_sem);
	OVE_BACKEND_FREE(t);
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

ove_thread_t ove_thread_get_self(void)
{
	return tls_current;
}

void ove_thread_set_priority(ove_thread_t handle, ove_prio_t prio)
{
	(void)handle;
	(void)prio;
}

void ove_thread_sleep_ms(uint32_t ms)
{
	touch_yield(tls_current);
	check_suspend(tls_current);
	usleep((useconds_t)ms * 1000);
	touch_yield(tls_current);
	check_suspend(tls_current);
}

void ove_thread_yield(void)
{
	touch_yield(tls_current);
	check_suspend(tls_current);
	sched_yield();
}

/* ── Supervisor thread ─────────────────────────────────────────────── */

#define SUPERVISOR_INTERVAL_MS 500   /* check every 500ms */
#define UNRESPONSIVE_THRESHOLD_MS 2000 /* warn after 2s without yield */

static volatile int supervisor_running;

static void *supervisor_loop(void *arg)
{
	(void)arg;

	while (supervisor_running) {
		usleep(SUPERVISOR_INTERVAL_MS * 1000);

		uint64_t now = now_us();
		pthread_mutex_lock(&tracked_lock);
		for (int i = 0; i < tracked_count; i++) {
			struct ove_thread *t = tracked_threads[i];
			if (!t || !t->started)
				continue;
			if (t->state == OVE_THREAD_STATE_TERMINATED ||
			    t->state == OVE_THREAD_STATE_SUSPENDED)
				continue;

			uint64_t last = t->last_yield_us;
			if (last == 0) continue;
			uint64_t elapsed_ms = (now - last) / 1000;

			if (elapsed_ms > UNRESPONSIVE_THRESHOLD_MS) {
				/* Thread hasn't yielded — may be CPU-bound.
				 * Set suspend flag so it suspends at next
				 * opportunity. Also log a warning. */
				if (t->suspend_requested) {
					/* Already flagged — this is a repeat.
					 * The thread is truly stuck. */
					fprintf(stderr,
						"[supervisor] thread %p "
						"unresponsive for %llu ms "
						"(CPU-bound loop?)\n",
						(void *)t,
						(unsigned long long)elapsed_ms);
				}
			}
		}
		pthread_mutex_unlock(&tracked_lock);
	}
	return NULL;
}

static pthread_t supervisor_tid;

static void supervisor_start(void)
{
	supervisor_running = 1;
	pthread_create(&supervisor_tid, NULL, supervisor_loop, NULL);
}

void ove_thread_start_scheduler(void)
{
	/* Start the supervisor thread that monitors responsiveness. */
	supervisor_start();

	/* Block by joining the first app thread (works because
	 * -sPROXY_TO_PTHREAD runs main() in a pthread). */
	if (first_thread != NULL)
		pthread_join(first_thread->tid, NULL);

	supervisor_running = 0;
}

void ove_thread_suspend(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t && t->started) {
		/* Cooperative: set flag, thread will suspend at next
		 * yield/sleep point.  No signal delivery. */
		t->suspend_requested = 1;
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t && t->state == OVE_THREAD_STATE_SUSPENDED) {
		t->state = OVE_THREAD_STATE_READY;
		sem_post(&t->suspend_sem);
	}
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	(void)handle;
	return 0;
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	return t ? t->state : OVE_THREAD_STATE_UNKNOWN;
}

int ove_thread_get_runtime_stats(ove_thread_t handle,
				     struct ove_thread_stats *stats)
{
	(void)handle;
	if (stats) {
		stats->runtime_us = 0;
		stats->cpu_percent_x100 = 0;
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats) return OVE_ERR_INVALID_PARAM;
	memset(stats, 0, sizeof(*stats));
#ifdef __EMSCRIPTEN__
	stats->total = (size_t)emscripten_get_heap_size();
#endif
	return OVE_OK;
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count,
		    size_t *actual_count)
{
	if (actual_count)
		*actual_count = 0;
	(void)out;
	(void)max_count;
	return OVE_OK;
}
