/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM/Emscripten timer backend.
 *
 * Replaces the POSIX timer_create/SIGEV_THREAD approach with a
 * single timer-manager pthread that maintains a sorted linked list
 * of armed timers.  Uses only pthreads + usleep — no Emscripten-
 * specific APIs, so this also works on native POSIX.
 */

#ifdef CONFIG_OVE_TIMER

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ── Timer manager state ───────────────────────────────────────────── */

static struct ove_timer   *active_head;  /* sorted by next_fire_us */
static pthread_mutex_t     mgr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t      mgr_cond = PTHREAD_COND_INITIALIZER;
static pthread_t           mgr_thread;
static int                 mgr_running;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Insert timer into the sorted active list (caller holds mgr_lock). */
static void list_insert(struct ove_timer *t)
{
	t->armed = 1;
	struct ove_timer **pp = &active_head;
	while (*pp && (*pp)->next_fire_us <= t->next_fire_us)
		pp = &(*pp)->next_active;
	t->next_active = *pp;
	*pp = t;
}

/* Remove timer from the active list (caller holds mgr_lock). */
static void list_remove(struct ove_timer *t)
{
	struct ove_timer **pp = &active_head;
	while (*pp) {
		if (*pp == t) {
			*pp = t->next_active;
			t->next_active = NULL;
			t->armed = 0;
			return;
		}
		pp = &(*pp)->next_active;
	}
}

/* ── Manager thread ────────────────────────────────────────────────── */

static void *timer_manager(void *arg)
{
	(void)arg;

	pthread_mutex_lock(&mgr_lock);
	while (mgr_running) {
		if (!active_head) {
			/* No timers — wait for one to be armed. */
			pthread_cond_wait(&mgr_cond, &mgr_lock);
			continue;
		}

		uint64_t now = now_us();
		uint64_t next = active_head->next_fire_us;

		if (now < next) {
			/* Sleep until the next timer fires. Use a timed
			 * wait so we wake if a sooner timer is inserted. */
			struct timespec ts;
			uint64_t wait_us = next - now;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += (time_t)(wait_us / 1000000ULL);
			ts.tv_nsec += (long)(wait_us % 1000000ULL) * 1000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&mgr_cond, &mgr_lock, &ts);
			continue;
		}

		/* Fire the head timer. */
		struct ove_timer *t = active_head;
		active_head = t->next_active;
		t->next_active = NULL;

		if (!t->one_shot) {
			/* Re-arm periodic timer. */
			t->next_fire_us = now + (uint64_t)t->period_ms * 1000;
			list_insert(t);
		} else {
			t->armed = 0;
		}

		/* Release lock while calling the callback to avoid
		 * deadlock if the callback calls timer APIs. */
		pthread_mutex_unlock(&mgr_lock);
		if (t->callback)
			t->callback(t, t->user_data);
		pthread_mutex_lock(&mgr_lock);
	}
	pthread_mutex_unlock(&mgr_lock);
	return NULL;
}

static void ensure_manager_started(void)
{
	if (mgr_running)
		return;
	mgr_running = 1;
	pthread_create(&mgr_thread, NULL, timer_manager, NULL);
}

/* ── Public API ────────────────────────────────────────────────────── */

int ove_timer_init(ove_timer_t *timer,
		       ove_timer_storage_t *storage,
		       ove_timer_fn callback,
		       void *user_data, uint32_t period_ms,
		       int one_shot)
{
	if (!timer || !storage || !callback) return OVE_ERR_INVALID_PARAM;
	struct ove_timer *t = (struct ove_timer *)storage;
	memset(t, 0, sizeof(*t));
	t->callback = callback;
	t->user_data = user_data;
	t->period_ms = period_ms;
	t->one_shot = one_shot;
	t->created = 1;
	*timer = t;

	ensure_manager_started();
	return OVE_OK;
}

void ove_timer_deinit(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (t && t->created) {
		pthread_mutex_lock(&mgr_lock);
		if (t->armed)
			list_remove(t);
		pthread_mutex_unlock(&mgr_lock);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_timer_create(ove_timer_t *timer,
			 ove_timer_fn callback,
			 void *user_data, uint32_t period_ms,
			 int one_shot)
{
	if (!timer || !callback) return OVE_ERR_INVALID_PARAM;
	struct ove_timer *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t) return OVE_ERR_NO_MEMORY;
	memset(t, 0, sizeof(*t));
	t->callback = callback;
	t->user_data = user_data;
	t->period_ms = period_ms;
	t->one_shot = one_shot;
	t->created = 1;
	*timer = t;

	ensure_manager_started();
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_timer_destroy(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (t) {
		if (t->created) {
			pthread_mutex_lock(&mgr_lock);
			if (t->armed)
				list_remove(t);
			pthread_mutex_unlock(&mgr_lock);
		}
		OVE_BACKEND_FREE(t);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_timer_start(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (!t) return OVE_ERR_INVALID_PARAM;

	pthread_mutex_lock(&mgr_lock);
	if (t->armed)
		list_remove(t);
	t->next_fire_us = now_us() + (uint64_t)t->period_ms * 1000;
	list_insert(t);
	pthread_cond_signal(&mgr_cond);
	pthread_mutex_unlock(&mgr_lock);
	return OVE_OK;
}

int ove_timer_stop(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (!t) return OVE_ERR_INVALID_PARAM;

	pthread_mutex_lock(&mgr_lock);
	if (t->armed)
		list_remove(t);
	pthread_mutex_unlock(&mgr_lock);
	return OVE_OK;
}

int ove_timer_reset(ove_timer_t timer)
{
	return ove_timer_start(timer);
}

#endif /* CONFIG_OVE_TIMER */
