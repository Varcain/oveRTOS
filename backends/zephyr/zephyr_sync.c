/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/sync.h"
#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/trace.h"
#include "ove_backend_common.h"
#include <zephyr/kernel.h>
static k_timeout_t ns_to_timeout(uint64_t ns)
{
	if (ove_timeout_is_forever(ns)) {
		return K_FOREVER;
	}
	return K_NSEC(ns);
}

/* ─── Mutex _init / _deinit ──────────────────────────────────────────── */

int ove_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	if (mtx == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	k_mutex_init(&storage->mtx);
	*mtx = storage;
	return OVE_OK;
}

void ove_mutex_deinit(ove_mutex_t mtx)
{
	(void)mtx;
}

/* ─── Mutex _create / _destroy ───────────────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_mutex_create(ove_mutex_t *mtx)
{
	int ret = ove_check_param(mtx);
	if (ret)
		return ret;

	struct ove_mutex *m = OVE_BACKEND_MALLOC(sizeof(*m));
	if (m == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	ret = ove_mutex_init(mtx, m);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(m);
	}
	return ret;
}

void ove_mutex_destroy(ove_mutex_t mtx)
{
	if (mtx != NULL) {
		ove_mutex_deinit(mtx);
		OVE_BACKEND_FREE(mtx);
	}
}
#endif /* OVE_HEAP_SYNC */

int ove_mutex_lock(ove_mutex_t mtx, uint64_t timeout_ns)
{
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_ENTER, mtx);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);

	int ret = k_mutex_lock(&mtx->mtx, ns_to_timeout(timeout_ns));

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_EXIT, mtx);
	return (ret == 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_mutex_unlock(ove_mutex_t mtx)
{
	k_mutex_unlock(&mtx->mtx);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_POST, mtx);
}

/* ─── Recursive Mutex _init ──────────────────────────────────────────── */

int ove_recursive_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	if (mtx == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	k_mutex_init(&storage->mtx);
	*mtx = storage;
	return OVE_OK;
}

/* ─── Recursive Mutex _create / _destroy ─────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_recursive_mutex_create(ove_mutex_t *mtx)
{
	if (mtx == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_mutex *m = OVE_BACKEND_MALLOC(sizeof(*m));
	if (m == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	int ret = ove_recursive_mutex_init(mtx, m);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(m);
	}
	return ret;
}

void ove_recursive_mutex_destroy(ove_mutex_t mtx)
{
	if (mtx != NULL) {
		OVE_BACKEND_FREE(mtx);
	}
}
#endif /* OVE_HEAP_SYNC */

int ove_recursive_mutex_lock(ove_mutex_t mtx, uint64_t timeout_ns)
{
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_ENTER, mtx);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);

	int ret = k_mutex_lock(&mtx->mtx, ns_to_timeout(timeout_ns));

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_EXIT, mtx);
	return (ret == 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_recursive_mutex_unlock(ove_mutex_t mtx)
{
	k_mutex_unlock(&mtx->mtx);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_POST, mtx);
}

/* ─── Semaphore _init / _deinit ──────────────────────────────────────── */

int ove_sem_init(ove_sem_t *sem, ove_sem_storage_t *storage, unsigned int initial, unsigned int max)
{
	if (sem == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	k_sem_init(&storage->sem, initial, max);
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	*sem = storage;
	return OVE_OK;
}

void ove_sem_deinit(ove_sem_t sem)
{
	(void)sem;
}

/* ─── Semaphore _create / _destroy ───────────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_sem_create(ove_sem_t *sem, unsigned int initial, unsigned int max)
{
	int ret = ove_check_param(sem);
	if (ret)
		return ret;

	struct ove_sem *s = OVE_BACKEND_MALLOC(sizeof(*s));
	if (s == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	ret = ove_sem_init(sem, s, initial, max);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(s);
	}
	return ret;
}

void ove_sem_destroy(ove_sem_t sem)
{
	if (sem != NULL) {
		ove_sem_deinit(sem);
		OVE_BACKEND_FREE(sem);
	}
}
#endif /* OVE_HEAP_SYNC */

int ove_sem_take(ove_sem_t sem, uint64_t timeout_ns)
{
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_WAIT_ENTER, sem);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);

	int ret = k_sem_take(&sem->sem, ns_to_timeout(timeout_ns));

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_WAIT_EXIT, sem);
	return (ret == 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_sem_give(ove_sem_t sem)
{
	k_sem_give(&sem->sem);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_POST, sem);
	if (sem->notify_cb != NULL) {
		sem->notify_cb(sem->notify_ud);
	}
}

int ove_sem_set_notify(ove_sem_t sem, ove_notify_cb cb, void *user_data)
{
	if (sem == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	sem->notify_cb = cb;
	sem->notify_ud = user_data;
	return OVE_OK;
}

/* ─── Event _init / _deinit ──────────────────────────────────────────── */

int ove_event_init(ove_event_t *evt, ove_event_storage_t *storage)
{
	if (evt == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	k_sem_init(&storage->sem, 0, 1);
	*evt = storage;
	return OVE_OK;
}

void ove_event_deinit(ove_event_t evt)
{
	(void)evt;
}

/* ─── Event _create / _destroy ───────────────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_event_create(ove_event_t *evt)
{
	int ret = ove_check_param(evt);
	if (ret)
		return ret;

	struct ove_event *e = OVE_BACKEND_MALLOC(sizeof(*e));
	if (e == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	ret = ove_event_init(evt, e);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(e);
	}
	return ret;
}

void ove_event_destroy(ove_event_t evt)
{
	if (evt != NULL) {
		ove_event_deinit(evt);
		OVE_BACKEND_FREE(evt);
	}
}
#endif /* OVE_HEAP_SYNC */

int ove_event_wait(ove_event_t evt, uint64_t timeout_ns)
{
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_ENTER, evt);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);

	int ret = k_sem_take(&evt->sem, ns_to_timeout(timeout_ns));

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_EXIT, evt);
	return (ret == 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_event_signal(ove_event_t evt)
{
	k_sem_give(&evt->sem);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_POST, evt);
}

void ove_event_signal_from_isr(ove_event_t evt)
{
	k_sem_give(&evt->sem);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_POST, evt);
}

/* ─── Condvar _init / _deinit ────────────────────────────────────────── */

int ove_condvar_init(ove_condvar_t *cv, ove_condvar_storage_t *storage)
{
	if (cv == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	k_condvar_init(&storage->cv);
	*cv = storage;
	return OVE_OK;
}

void ove_condvar_deinit(ove_condvar_t cv)
{
	(void)cv;
}

/* ─── Condvar _create / _destroy ─────────────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_condvar_create(ove_condvar_t *cv)
{
	int ret = ove_check_param(cv);
	if (ret)
		return ret;

	struct ove_condvar *c = OVE_BACKEND_MALLOC(sizeof(*c));
	if (c == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	ret = ove_condvar_init(cv, c);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(c);
	}
	return ret;
}

void ove_condvar_destroy(ove_condvar_t cv)
{
	if (cv != NULL) {
		ove_condvar_deinit(cv);
		OVE_BACKEND_FREE(cv);
	}
}
#endif /* OVE_HEAP_SYNC */

int ove_condvar_wait(ove_condvar_t cv, ove_mutex_t mtx, uint64_t timeout_ns)
{
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_ENTER, cv);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);

	int ret = k_condvar_wait(&cv->cv, &mtx->mtx, ns_to_timeout(timeout_ns));

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_EXIT, cv);
	return (ret == 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_condvar_signal(ove_condvar_t cv)
{
	k_condvar_signal(&cv->cv);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_POST, cv);
}

void ove_condvar_broadcast(ove_condvar_t cv)
{
	k_condvar_broadcast(&cv->cv);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_POST, cv);
}
