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
#ifdef CONFIG_OVE_ASYNC
/* Forward declarations from posix_irq.c — used to mark the SIGEV_THREAD
 * dispatcher as ISR context so async wakers dispatch through
 * ove_event_signal_from_isr correctly. Only declared when async is
 * compiled in; without it the symbols don't exist and the brackets
 * collapse to no-ops at the macro level. */
void posix_irq_enter_isr(void);
void posix_irq_leave_isr(void);
#define OVE_POSIX_ISR_ENTER() posix_irq_enter_isr()
#define OVE_POSIX_ISR_LEAVE() posix_irq_leave_isr()
#else
#define OVE_POSIX_ISR_ENTER() ((void)0)
#define OVE_POSIX_ISR_LEAVE() ((void)0)
#endif

static void timer_thread_handler(union sigval sv)
{
	struct ove_timer *t = sv.sival_ptr;
	if (t && t->callback) {
		OVE_POSIX_ISR_ENTER();
		t->callback(t, t->user_data);
		OVE_POSIX_ISR_LEAVE();
	}
}

/* SIGEV_THREAD spawns a fresh pthread per timer firing.  glibc's default
 * stack for the dispatch thread is too small for sanitizer-instrumented
 * builds (TSan needs ~140 KB; ASan ~96 KB; the default is 64 KB).
 * Provide a 256 KB stack via pthread_attr_t so all sanitizer flavours
 * fit; harmless on a regular release build. */
static pthread_attr_t s_timer_thread_attr;
static int s_timer_thread_attr_initialized;

static pthread_attr_t *get_timer_thread_attr(void)
{
	if (!s_timer_thread_attr_initialized) {
		if (pthread_attr_init(&s_timer_thread_attr) == 0) {
			(void)pthread_attr_setstacksize(&s_timer_thread_attr, 256u * 1024u);
			s_timer_thread_attr_initialized = 1;
		}
	}
	return s_timer_thread_attr_initialized ? &s_timer_thread_attr : NULL;
}

int ove_timer_init_ns(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		      void *user_data, uint64_t period_ns, int one_shot)
{
	if (!timer || !storage || !callback)
		return OVE_ERR_INVALID_PARAM;
	struct ove_timer *t = (struct ove_timer *)storage;
	memset(t, 0, sizeof(*t));
	t->callback = callback;
	t->user_data = user_data;
	t->period_ns = period_ns;
	t->one_shot = one_shot;

	struct sigevent sev;
	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = timer_thread_handler;
	sev.sigev_value.sival_ptr = t;
	sev.sigev_notify_attributes = get_timer_thread_attr();

	if (timer_create(CLOCK_MONOTONIC, &sev, &t->posix_timer) != 0) {
		return OVE_ERR_NO_MEMORY;
	}
	t->created = 1;
	*timer = t;
	return OVE_OK;
}

int ove_timer_init(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		   void *user_data, uint32_t period_ms, int one_shot)
{
	return ove_timer_init_ns(timer, storage, callback, user_data,
				 (uint64_t)period_ms * 1000000ULL, one_shot);
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
int ove_timer_create_ns(ove_timer_t *timer, ove_timer_fn callback, void *user_data,
			uint64_t period_ns, int one_shot)
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
	t->period_ns = period_ns;
	t->one_shot = one_shot;

	struct sigevent sev;
	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = timer_thread_handler;
	sev.sigev_value.sival_ptr = t;
	sev.sigev_notify_attributes = get_timer_thread_attr();

	if (timer_create(CLOCK_MONOTONIC, &sev, &t->posix_timer) != 0) {
		OVE_BACKEND_FREE(t);
		return OVE_ERR_NO_MEMORY;
	}
	t->created = 1;
	*timer = t;
	return OVE_OK;
}

int ove_timer_create(ove_timer_t *timer, ove_timer_fn callback, void *user_data, uint32_t period_ms,
		     int one_shot)
{
	return ove_timer_create_ns(timer, callback, user_data,
				   (uint64_t)period_ms * 1000000ULL, one_shot);
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
	its.it_value.tv_sec = (time_t)(t->period_ns / 1000000000ULL);
	its.it_value.tv_nsec = (long)(t->period_ns % 1000000000ULL);
	/* timer_settime treats an all-zero it_value as "disarm" — guard
	 * against an accidental no-op when the caller passes period_ns=0
	 * by clamping to 1 ns. The Embassy time driver never schedules a
	 * zero-duration alarm, but this also catches stale state from a
	 * failed reprogram. */
	if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) {
		its.it_value.tv_nsec = 1;
	}
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
