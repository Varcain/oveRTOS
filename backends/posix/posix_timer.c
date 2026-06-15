/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#ifdef CONFIG_OVE_ASYNC
/* Forward declarations from posix_irq.c — used to mark the timer
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

/* The dispatcher runs the user callback, so it needs a generous stack:
 * sanitizer-instrumented builds want ~140 KB (TSan) / ~96 KB (ASan), and
 * the Zig 0.16 toolchain's TLS footprint inflates glibc's dynamic
 * stack-min above 256 KB on x86_64-linux.  512 KB covers every flavour. */
static pthread_attr_t s_timer_thread_attr;
static int s_timer_thread_attr_initialized;

static pthread_attr_t *get_timer_thread_attr(void)
{
	if (!s_timer_thread_attr_initialized) {
		if (pthread_attr_init(&s_timer_thread_attr) == 0) {
			(void)pthread_attr_setstacksize(&s_timer_thread_attr, 512u * 1024u);
			s_timer_thread_attr_initialized = 1;
		}
	}
	return s_timer_thread_attr_initialized ? &s_timer_thread_attr : NULL;
}

static void timespec_add_ns(struct timespec *ts, uint64_t ns)
{
	ts->tv_sec += (time_t)(ns / 1000000000ULL);
	ts->tv_nsec += (long)(ns % 1000000000ULL);
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

/* Owned dispatcher: self-times each firing and runs the callback.  Because
 * teardown pthread_join()s this thread, no callback can run after the timer
 * storage is deleted/freed — eliminating the SIGEV_THREAD UAF window. */
static void *timer_dispatch(void *arg)
{
	struct ove_timer *t = arg;
	pthread_mutex_lock(&t->lock);
	for (;;) {
		/* Idle until armed or torn down. */
		while (!t->armed && !t->stop)
			pthread_cond_wait(&t->cond, &t->lock);
		if (t->stop)
			break;

		/* Program the next absolute deadline from now. */
		struct timespec deadline;
		clock_gettime(CLOCK_MONOTONIC, &deadline);
		timespec_add_ns(&deadline, t->period_ns ? t->period_ns : 1);
		t->reprogram = 0;

		/* Wait for the deadline, or wake early on a state change. */
		int fired = 0;
		while (t->armed && !t->stop && !t->reprogram) {
			if (pthread_cond_timedwait(&t->cond, &t->lock, &deadline) == ETIMEDOUT) {
				fired = 1;
				break;
			}
		}
		if (t->stop)
			break;
		if (!fired || t->reprogram || !t->armed)
			continue; /* disarmed / reprogrammed before firing */

		if (t->one_shot)
			t->armed = 0;
		ove_timer_fn cb = t->callback;
		void *ud = t->user_data;
		/* Run the callback with the lock released so it may call
		 * stop/reset on its own timer without deadlocking.  The
		 * dispatcher cannot exit mid-callback, so teardown's join still
		 * waits for the callback to return before freeing. */
		pthread_mutex_unlock(&t->lock);
		if (cb) {
			OVE_POSIX_ISR_ENTER();
			cb(t, ud);
			OVE_POSIX_ISR_LEAVE();
		}
		pthread_mutex_lock(&t->lock);
	}
	pthread_mutex_unlock(&t->lock);
	return NULL;
}

static int timer_setup(struct ove_timer *t, ove_timer_fn callback, void *user_data,
		       uint64_t period_ns, int one_shot)
{
	memset(t, 0, sizeof(*t));
	t->callback = callback;
	t->user_data = user_data;
	t->period_ns = period_ns;
	t->one_shot = one_shot;

	pthread_condattr_t cattr;
	if (pthread_condattr_init(&cattr) != 0)
		return OVE_ERR_NO_MEMORY;
	(void)pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	int rc = pthread_cond_init(&t->cond, &cattr);
	pthread_condattr_destroy(&cattr);
	if (rc != 0)
		return OVE_ERR_NO_MEMORY;
	if (pthread_mutex_init(&t->lock, NULL) != 0) {
		pthread_cond_destroy(&t->cond);
		return OVE_ERR_NO_MEMORY;
	}
	if (pthread_create(&t->thread, get_timer_thread_attr(), timer_dispatch, t) != 0) {
		pthread_mutex_destroy(&t->lock);
		pthread_cond_destroy(&t->cond);
		return OVE_ERR_NO_MEMORY;
	}
	t->created = 1;
	return OVE_OK;
}

/* Stop the dispatcher and reclaim its sync primitives.  Joins the thread so
 * any in-flight callback has returned before the caller frees the storage. */
static void timer_teardown(struct ove_timer *t)
{
	pthread_mutex_lock(&t->lock);
	t->stop = 1;
	t->armed = 0;
	pthread_cond_signal(&t->cond);
	pthread_mutex_unlock(&t->lock);
	pthread_join(t->thread, NULL);
	pthread_mutex_destroy(&t->lock);
	pthread_cond_destroy(&t->cond);
}

int ove_timer_init_ns(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		      void *user_data, uint64_t period_ns, int one_shot)
{
	if (!timer || !storage || !callback)
		return OVE_ERR_INVALID_PARAM;
	struct ove_timer *t = (struct ove_timer *)storage;
	int rc = timer_setup(t, callback, user_data, period_ns, one_shot);
	if (rc != OVE_OK)
		return rc;
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
	if (t && t->created) {
		timer_teardown(t);
		t->created = 0;
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_timer_create_ns(ove_timer_t *timer, ove_timer_fn callback, void *user_data,
			uint64_t period_ns, int one_shot)
{
	if (!timer || !callback)
		return OVE_ERR_INVALID_PARAM;
	struct ove_timer *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t)
		return OVE_ERR_NO_MEMORY;
	int rc = timer_setup(t, callback, user_data, period_ns, one_shot);
	if (rc != OVE_OK) {
		OVE_BACKEND_FREE(t);
		return rc;
	}
	*timer = t;
	return OVE_OK;
}

int ove_timer_create(ove_timer_t *timer, ove_timer_fn callback, void *user_data, uint32_t period_ms,
		     int one_shot)
{
	return ove_timer_create_ns(timer, callback, user_data, (uint64_t)period_ms * 1000000ULL,
				   one_shot);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_timer_destroy(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (t) {
		if (t->created)
			timer_teardown(t);
		OVE_BACKEND_FREE(t);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_timer_start(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (!t)
		return OVE_ERR_INVALID_PARAM;
	pthread_mutex_lock(&t->lock);
	t->armed = 1;
	t->reprogram = 1; /* (re)compute the deadline from now */
	pthread_cond_signal(&t->cond);
	pthread_mutex_unlock(&t->lock);
	return OVE_OK;
}

int ove_timer_stop(ove_timer_t timer)
{
	struct ove_timer *t = timer;
	if (!t)
		return OVE_ERR_INVALID_PARAM;
	pthread_mutex_lock(&t->lock);
	t->armed = 0;
	pthread_cond_signal(&t->cond);
	pthread_mutex_unlock(&t->lock);
	return OVE_OK;
}

int ove_timer_reset(ove_timer_t timer)
{
	return ove_timer_start(timer);
}

int ove_timer_set_period_ns(ove_timer_t timer, uint64_t period_ns)
{
	struct ove_timer *t = timer;
	if (!t)
		return OVE_ERR_INVALID_PARAM;
	pthread_mutex_lock(&t->lock);
	t->period_ns = period_ns;
	t->armed = 1;
	t->reprogram = 1;
	pthread_cond_signal(&t->cond);
	pthread_mutex_unlock(&t->lock);
	return OVE_OK;
}
