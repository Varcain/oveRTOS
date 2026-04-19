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
#include <semaphore.h>
#include <time.h>
#include <errno.h>
#include <string.h>

/* ---------- helpers ---------- */

static void ms_to_abstime(uint32_t timeout_ms, struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += timeout_ms / 1000;
	ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

/* ---------- Mutex ---------- */

int ove_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	int ret = ove_check_param(mtx);
	if (ret) return ret;
	if (!storage) return OVE_ERR_INVALID_PARAM;
	struct ove_mutex *m = (struct ove_mutex *)storage;
	pthread_mutex_init(&m->mtx, NULL);
	*mtx = m;
	return OVE_OK;
}

void ove_mutex_deinit(ove_mutex_t mtx)
{
	if (mtx) {
		pthread_mutex_destroy(&mtx->mtx);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_mutex_create(ove_mutex_t *mtx)
{
	int ret = ove_check_param(mtx);
	if (ret) return ret;
	struct ove_mutex *m = OVE_BACKEND_MALLOC(sizeof(*m));
	if (m == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	pthread_mutex_init(&m->mtx, NULL);
	*mtx = m;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_mutex_destroy(ove_mutex_t mtx)
{
	if (mtx) {
		pthread_mutex_destroy(&mtx->mtx);
		OVE_BACKEND_FREE(mtx);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_mutex_lock(ove_mutex_t mtx, uint32_t timeout_ms)
{
	if (mtx == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (ove_timeout_is_forever(timeout_ms)) {
		pthread_mutex_lock(&mtx->mtx);
		return OVE_OK;
	}
	struct timespec ts;
	ms_to_abstime(timeout_ms, &ts);
	int ret = pthread_mutex_timedlock(&mtx->mtx, &ts);
	return (ret == ETIMEDOUT) ? OVE_ERR_TIMEOUT : OVE_OK;
}

void ove_mutex_unlock(ove_mutex_t mtx)
{
	if (mtx) {
		pthread_mutex_unlock(&mtx->mtx);
	}
}

/* ---------- Semaphore ---------- */

int ove_sem_init(ove_sem_t *sem, ove_sem_storage_t *storage,
		     unsigned int initial, unsigned int max)
{
	int ret = ove_check_param(sem);
	if (ret) return ret;
	if (!storage) return OVE_ERR_INVALID_PARAM;
	(void)max;
	struct ove_sem *s = (struct ove_sem *)storage;
	sem_init(&s->sem, 0, initial);
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

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_sem_create(ove_sem_t *sem, unsigned int initial,
		       unsigned int max)
{
	int ret = ove_check_param(sem);
	if (ret) return ret;
	(void)max;
	struct ove_sem *s = OVE_BACKEND_MALLOC(sizeof(*s));
	if (s == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	sem_init(&s->sem, 0, initial);
	*sem = s;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_sem_destroy(ove_sem_t sem)
{
	struct ove_sem *s = sem;
	if (s) {
		sem_destroy(&s->sem);
		OVE_BACKEND_FREE(s);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_sem_take(ove_sem_t sem, uint32_t timeout_ms)
{
	struct ove_sem *s = sem;
	if (s == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (ove_timeout_is_forever(timeout_ms)) {
		sem_wait(&s->sem);
		return OVE_OK;
	}
	struct timespec ts;
	ms_to_abstime(timeout_ms, &ts);
	int ret = sem_timedwait(&s->sem, &ts);
	if (ret == 0)
		return OVE_OK;
	if (errno == ETIMEDOUT)
		return OVE_ERR_TIMEOUT;
	if (errno == EINVAL)
		return OVE_ERR_INVALID_PARAM;
	return OVE_ERR_NOT_SUPPORTED;
}

void ove_sem_give(ove_sem_t sem)
{
	struct ove_sem *s = sem;
	if (s) {
		sem_post(&s->sem);
	}
}

/* ---------- Event (binary semaphore) ---------- */

int ove_event_init(ove_event_t *evt, ove_event_storage_t *storage)
{
	int ret = ove_check_param(evt);
	if (ret) return ret;
	if (!storage) return OVE_ERR_INVALID_PARAM;
	struct ove_event *e = (struct ove_event *)storage;
	memset(e, 0, sizeof(*e));
	pthread_mutex_init(&e->lock, NULL);
	pthread_cond_init(&e->cond, NULL);
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

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_event_create(ove_event_t *evt)
{
	int ret = ove_check_param(evt);
	if (ret) return ret;
	struct ove_event *e = OVE_BACKEND_MALLOC(sizeof(*e));
	if (e == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(e, 0, sizeof(*e));
	pthread_mutex_init(&e->lock, NULL);
	pthread_cond_init(&e->cond, NULL);
	*evt = e;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_event_destroy(ove_event_t evt)
{
	struct ove_event *e = evt;
	if (e) {
		pthread_mutex_destroy(&e->lock);
		pthread_cond_destroy(&e->cond);
		OVE_BACKEND_FREE(e);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_event_wait(ove_event_t evt, uint32_t timeout_ms)
{
	struct ove_event *e = evt;
	if (e == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&e->lock);
	if (ove_timeout_is_forever(timeout_ms)) {
		while (!e->signaled) {
			pthread_cond_wait(&e->cond, &e->lock);
		}
	} else {
		struct timespec ts;
		ms_to_abstime(timeout_ms, &ts);
		while (!e->signaled) {
			int ret = pthread_cond_timedwait(&e->cond, &e->lock,
							&ts);
			if (ret == ETIMEDOUT) {
				/* Re-check under lock: signaler may have fired
				 * between timeout expiry and mutex re-acquire. */
				if (!e->signaled) {
					pthread_mutex_unlock(&e->lock);
					return OVE_ERR_TIMEOUT;
				}
				break;
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
		pthread_mutex_lock(&e->lock);
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

int ove_recursive_mutex_init(ove_mutex_t *mtx,
				 ove_mutex_storage_t *storage)
{
	if (!mtx || !storage) return OVE_ERR_INVALID_PARAM;
	struct ove_mutex *m = (struct ove_mutex *)storage;
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&m->mtx, &attr);
	pthread_mutexattr_destroy(&attr);
	*mtx = m;
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_recursive_mutex_create(ove_mutex_t *mtx)
{
	struct ove_mutex *m = OVE_BACKEND_MALLOC(sizeof(*m));
	if (m == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&m->mtx, &attr);
	pthread_mutexattr_destroy(&attr);
	*mtx = m;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_recursive_mutex_lock(ove_mutex_t mtx,
				 uint32_t timeout_ms)
{
	return ove_mutex_lock(mtx, timeout_ms);
}

void ove_recursive_mutex_unlock(ove_mutex_t mtx)
{
	ove_mutex_unlock(mtx);
}

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_recursive_mutex_destroy(ove_mutex_t mtx)
{
	ove_mutex_destroy(mtx);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ---------- Condition Variable ---------- */

int ove_condvar_init(ove_condvar_t *cv,
			 ove_condvar_storage_t *storage)
{
	int ret = ove_check_param(cv);
	if (ret) return ret;
	if (!storage) return OVE_ERR_INVALID_PARAM;
	struct ove_condvar *c = (struct ove_condvar *)storage;
	pthread_cond_init(&c->cond, NULL);
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

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_condvar_create(ove_condvar_t *cv)
{
	int ret = ove_check_param(cv);
	if (ret) return ret;
	struct ove_condvar *c = OVE_BACKEND_MALLOC(sizeof(*c));
	if (c == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	pthread_cond_init(&c->cond, NULL);
	*cv = c;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_condvar_destroy(ove_condvar_t cv)
{
	struct ove_condvar *c = cv;
	if (c) {
		pthread_cond_destroy(&c->cond);
		OVE_BACKEND_FREE(c);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_condvar_wait(ove_condvar_t cv, ove_mutex_t mtx,
			 uint32_t timeout_ms)
{
	struct ove_condvar *c = cv;
	if (c == NULL || mtx == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (ove_timeout_is_forever(timeout_ms)) {
		pthread_cond_wait(&c->cond, &mtx->mtx);
		return OVE_OK;
	}
	struct timespec ts;
	ms_to_abstime(timeout_ms, &ts);
	int ret = pthread_cond_timedwait(&c->cond, &mtx->mtx, &ts);
	return (ret == ETIMEDOUT) ? OVE_ERR_TIMEOUT : OVE_OK;
}

void ove_condvar_signal(ove_condvar_t cv)
{
	struct ove_condvar *c = cv;
	if (c) {
		pthread_cond_signal(&c->cond);
	}
}

void ove_condvar_broadcast(ove_condvar_t cv)
{
	struct ove_condvar *c = cv;
	if (c) {
		pthread_cond_broadcast(&c->cond);
	}
}
