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

void ove_recursive_mutex_deinit(ove_mutex_t mtx)
{
	ove_mutex_deinit(mtx);
}

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
