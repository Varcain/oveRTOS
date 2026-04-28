/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include "posix_sleep.h"
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
static void timer_thread_handler(union sigval sv)
{
	struct ove_timer *t = sv.sival_ptr;
	if (t && t->callback) {
		t->callback(t, t->user_data);
	}
}

int ove_timer_init(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		   void *user_data, uint32_t period_ms, int one_shot)
{
	if (!timer || !storage || !callback)
		return OVE_ERR_INVALID_PARAM;
	struct ove_timer *t = (struct ove_timer *)storage;
	memset(t, 0, sizeof(*t));
	t->callback = callback;
	t->user_data = user_data;
	t->period_ms = period_ms;
	t->one_shot = one_shot;

	struct sigevent sev;
	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = timer_thread_handler;
	sev.sigev_value.sival_ptr = t;

	if (timer_create(CLOCK_MONOTONIC, &sev, &t->posix_timer) != 0) {
		return OVE_ERR_NO_MEMORY;
	}
	t->created = 1;
	*timer = t;
	return OVE_OK;
}

void ove_timer_deinit(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (t) {
		if (t->created) {
			/* Disarm first, then let any in-flight SIGEV_THREAD
			 * callback drain before deleting the timer. */
			struct itimerspec its = {{0, 0}, {0, 0}};
			timer_settime(t->posix_timer, 0, &its, NULL);
			posix_sleep_ns(5000000ULL);
			timer_delete(t->posix_timer);
		}
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_timer_create(ove_timer_t *timer, ove_timer_fn callback, void *user_data, uint32_t period_ms,
		     int one_shot)
{
	if (!timer || !callback)
		return OVE_ERR_INVALID_PARAM;
	struct ove_timer *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(t, 0, sizeof(*t));
	t->callback = callback;
	t->user_data = user_data;
	t->period_ms = period_ms;
	t->one_shot = one_shot;

	struct sigevent sev;
	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = timer_thread_handler;
	sev.sigev_value.sival_ptr = t;

	if (timer_create(CLOCK_MONOTONIC, &sev, &t->posix_timer) != 0) {
		OVE_BACKEND_FREE(t);
		return OVE_ERR_NO_MEMORY;
	}
	t->created = 1;
	*timer = t;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_timer_destroy(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (t) {
		if (t->created) {
			/* Disarm first, then let any in-flight SIGEV_THREAD
			 * callback drain before deleting the timer. */
			struct itimerspec its = {{0, 0}, {0, 0}};
			timer_settime(t->posix_timer, 0, &its, NULL);
			posix_sleep_ns(5000000ULL);
			timer_delete(t->posix_timer);
		}
		OVE_BACKEND_FREE(t);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_timer_start(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (!t) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct itimerspec its;
	its.it_value.tv_sec = t->period_ms / 1000;
	its.it_value.tv_nsec = (long)(t->period_ms % 1000) * 1000000L;
	if (t->one_shot) {
		its.it_interval.tv_sec = 0;
		its.it_interval.tv_nsec = 0;
	} else {
		its.it_interval = its.it_value;
	}
	if (timer_settime(t->posix_timer, 0, &its, NULL) != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

int ove_timer_stop(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (!t) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct itimerspec its = {{0, 0}, {0, 0}};
	timer_settime(t->posix_timer, 0, &its, NULL);
	return OVE_OK;
}

int ove_timer_reset(ove_timer_t timer)
{
	return ove_timer_start(timer);
}
