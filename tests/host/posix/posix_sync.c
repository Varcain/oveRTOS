/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove/trace.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
/* ---------- helpers ---------- */

static int mutex_result(int status)
{
	/* Recursive depth exhaustion does not mean deadline expiry. */
	return status == EAGAIN ? OVE_ERR_BUSY : ove_errno_to_ove(status);
}

static int condvar_result(int status, bool timed)
{
	if (status == ETIMEDOUT)
		return timed ? OVE_ERR_TIMEOUT : OVE_ERR_BUSY;
	return mutex_result(status);
}

static void ns_to_abstime(uint64_t timeout_ns, struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += (time_t)(timeout_ns / 1000000000ULL);
	ts->tv_nsec += (long)(timeout_ns % 1000000000ULL);
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

/* ---------- Mutex ---------- */

int ove_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	if (mtx == NULL || storage == NULL)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mutex *m = (struct ove_mutex *)storage;
	int rc = pthread_mutex_init(&m->mtx, NULL);
	if (rc)
		return ove_errno_to_ove(rc);
	*mtx = m;
	return OVE_OK;
}

void ove_mutex_deinit(ove_mutex_t mtx)
{
	if (mtx) {
		pthread_mutex_destroy(&mtx->mtx);
	}
}

int ove_mutex_lock(ove_mutex_t mtx, uint64_t timeout_ns)
{
	if (ove_timeout_is_forever(timeout_ns)) {
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_ENTER, mtx);
		ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
		int ret = pthread_mutex_lock(&mtx->mtx);
		ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_EXIT, mtx);
		return mutex_result(ret);
	}
	struct timespec ts;
	ns_to_abstime(timeout_ns, &ts);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_ENTER, mtx);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
	int ret = pthread_mutex_timedlock(&mtx->mtx, &ts);
	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_EXIT, mtx);
	return mutex_result(ret);
}

void ove_mutex_unlock(ove_mutex_t mtx)
{
	if (mtx) {
		pthread_mutex_unlock(&mtx->mtx);
	}
}

/* ---------- Semaphore ---------- */

int ove_sem_init(ove_sem_t *sem, ove_sem_storage_t *storage, unsigned int initial, unsigned int max)
{
	if (sem == NULL || storage == NULL || max == 0 || initial > max || max > SEM_VALUE_MAX)
		return OVE_ERR_INVALID_PARAM;
	struct ove_sem *s = (struct ove_sem *)storage;
	if (sem_init(&s->sem, 0, initial) != 0)
		return ove_errno_to_ove(errno);
	s->notify_cb = NULL;
	s->notify_ud = NULL;
	*sem = s;
	return OVE_OK;
}

void ove_sem_deinit(ove_sem_t sem)
{
	struct ove_sem *s = sem;
	if (s) {
		sem_destroy(&s->sem);
	}
}

int ove_sem_take(ove_sem_t sem, uint64_t timeout_ns)
{
	struct ove_sem *s = sem;
	if (s == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (ove_timeout_is_forever(timeout_ns)) {
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_WAIT_ENTER, s);
		ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
		/* Retry on EINTR: a signal (suspend/profiler) must not make the
		 * wait return without actually acquiring the semaphore. */
		int rc, error;
		do {
			rc = sem_wait(&s->sem);
			error = rc == 0 ? 0 : errno;
		} while (rc != 0 && error == EINTR);
		ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_WAIT_EXIT, s);
		return ove_errno_to_ove(error);
	}
	struct timespec ts;
	ns_to_abstime(timeout_ns, &ts);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_WAIT_ENTER, s);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
	/* Retry on EINTR; the absolute deadline `ts` keeps the total wait
	 * bounded across signal interruptions. */
	int ret, error;
	do {
		ret = sem_timedwait(&s->sem, &ts);
		error = ret == 0 ? 0 : errno;
	} while (ret != 0 && error == EINTR);
	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_WAIT_EXIT, s);
	return ove_errno_to_ove(error);
}

void ove_sem_give(ove_sem_t sem)
{
	struct ove_sem *s = sem;
	if (s) {
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_POST, s);
		sem_post(&s->sem);
		if (s->notify_cb != NULL) {
			s->notify_cb(s->notify_ud);
		}
	}
}

int ove_sem_set_notify(ove_sem_t sem, ove_notify_cb cb, void *user_data)
{
	struct ove_sem *s = sem;
	if (s == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	s->notify_cb = cb;
	s->notify_ud = user_data;
	return OVE_OK;
}

/* ---------- Event (binary semaphore) ---------- */

int ove_event_init(ove_event_t *evt, ove_event_storage_t *storage)
{
	if (evt == NULL || storage == NULL)
		return OVE_ERR_INVALID_PARAM;
	struct ove_event *e = (struct ove_event *)storage;
	int rc = pthread_mutex_init(&e->lock, NULL);
	if (rc)
		return ove_errno_to_ove(rc);
	rc = pthread_cond_init(&e->cond, NULL);
	if (rc) {
		pthread_mutex_destroy(&e->lock);
		return ove_errno_to_ove(rc);
	}
	e->signaled = 0;
	*evt = e;
	return OVE_OK;
}

void ove_event_deinit(ove_event_t evt)
{
	struct ove_event *e = evt;
	if (e) {
		pthread_mutex_destroy(&e->lock);
		pthread_cond_destroy(&e->cond);
	}
}

int ove_event_wait(ove_event_t evt, uint64_t timeout_ns)
{
	struct ove_event *e = evt;
	if (e == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = pthread_mutex_lock(&e->lock);
	if (ret)
		return condvar_result(ret, false);
	if (ove_timeout_is_forever(timeout_ns)) {
		while (!e->signaled) {
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_ENTER, e);
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			ret = pthread_cond_wait(&e->cond, &e->lock);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_EXIT, e);
			if (ret) {
				/* Defined errors preserve ownership of this default,
				 * nonrobust mutex. Release it before returning. */
				pthread_mutex_unlock(&e->lock);
				return condvar_result(ret, false);
			}
		}
	} else {
		struct timespec ts;
		ns_to_abstime(timeout_ns, &ts);
		while (!e->signaled) {
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_ENTER, e);
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			int ret = pthread_cond_timedwait(&e->cond, &e->lock, &ts);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_EXIT, e);
			if (ret == ETIMEDOUT) {
				/* Re-check under lock: signaler may have fired
				 * between timeout expiry and mutex re-acquire. */
				if (!e->signaled) {
					pthread_mutex_unlock(&e->lock);
					return OVE_ERR_TIMEOUT;
				}
				break;
			}
			if (ret) {
				/* The internal default nonrobust mutex remains held
				 * after a defined condition-wait error. */
				pthread_mutex_unlock(&e->lock);
				return condvar_result(ret, false);
			}
		}
	}
	e->signaled = 0;
	pthread_mutex_unlock(&e->lock);
	return OVE_OK;
}

void ove_event_signal(ove_event_t evt)
{
	struct ove_event *e = evt;
	if (e) {
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_POST, e);
		if (pthread_mutex_lock(&e->lock) != 0)
			return;
		e->signaled = 1;
		pthread_cond_signal(&e->cond);
		pthread_mutex_unlock(&e->lock);
	}
}

void ove_event_signal_from_isr(ove_event_t evt)
{
	ove_event_signal(evt);
}

/* ---------- Recursive Mutex ---------- */

int ove_recursive_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	if (mtx == NULL || storage == NULL)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mutex *m = (struct ove_mutex *)storage;
	pthread_mutexattr_t attr;
	int rc = pthread_mutexattr_init(&attr);
	if (rc)
		return ove_errno_to_ove(rc);
	rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (rc == 0)
		rc = pthread_mutex_init(&m->mtx, &attr);
	pthread_mutexattr_destroy(&attr);
	if (rc)
		return ove_errno_to_ove(rc);
	*mtx = m;
	return OVE_OK;
}

void ove_recursive_mutex_deinit(ove_mutex_t mtx)
{
	ove_mutex_deinit(mtx);
}

int ove_recursive_mutex_lock(ove_mutex_t mtx, uint64_t timeout_ns)
{
	return ove_mutex_lock(mtx, timeout_ns);
}

void ove_recursive_mutex_unlock(ove_mutex_t mtx)
{
	ove_mutex_unlock(mtx);
}

/* ---------- Condition Variable ---------- */

int ove_condvar_init(ove_condvar_t *cv, ove_condvar_storage_t *storage)
{
	if (cv == NULL || storage == NULL)
		return OVE_ERR_INVALID_PARAM;
	struct ove_condvar *c = (struct ove_condvar *)storage;
	int rc = pthread_cond_init(&c->cond, NULL);
	if (rc)
		return ove_errno_to_ove(rc);
	*cv = c;
	return OVE_OK;
}

void ove_condvar_deinit(ove_condvar_t cv)
{
	struct ove_condvar *c = cv;
	if (c) {
		pthread_cond_destroy(&c->cond);
	}
}

int ove_condvar_wait(ove_condvar_t cv, ove_mutex_t mtx, uint64_t timeout_ns)
{
	struct ove_condvar *c = cv;
	if (ove_timeout_is_forever(timeout_ns)) {
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_ENTER, c);
		ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
		int ret = pthread_cond_wait(&c->cond, &mtx->mtx);
		ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_EXIT, c);
		return condvar_result(ret, false);
	}
	struct timespec ts;
	ns_to_abstime(timeout_ns, &ts);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_ENTER, c);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
	int ret = pthread_cond_timedwait(&c->cond, &mtx->mtx, &ts);
	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_EXIT, c);
	return condvar_result(ret, true);
}

void ove_condvar_signal(ove_condvar_t cv)
{
	struct ove_condvar *c = cv;
	if (c) {
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_POST, c);
		pthread_cond_signal(&c->cond);
	}
}

void ove_condvar_broadcast(ove_condvar_t cv)
{
	struct ove_condvar *c = cv;
	if (c) {
		OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_POST, c);
		pthread_cond_broadcast(&c->cond);
	}
}
