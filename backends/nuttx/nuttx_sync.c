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
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include "ove_ns_to_ticks.h"
#include <errno.h>

/* ─── Mutex _init / _deinit ──────────────────────────────────────────── */

int ove_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	if (mtx == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	nxmutex_init(&storage->mtx);
	*mtx = storage;
	return OVE_OK;
}

void ove_mutex_deinit(ove_mutex_t mtx)
{
	if (mtx != NULL) {
		nxmutex_destroy(&mtx->mtx);
	}
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

	int ret;
	if (ove_timeout_is_forever(timeout_ns)) {
		while ((ret = nxmutex_lock(&mtx->mtx)) == -EINTR)
			;
	} else {
		ret = nxmutex_ticklock(&mtx->mtx, ove_ns_to_ticks(timeout_ns));
	}

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_EXIT, mtx);
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_mutex_unlock(ove_mutex_t mtx)
{
	nxmutex_unlock(&mtx->mtx);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_POST, mtx);
}

/* ─── Recursive Mutex _init ──────────────────────────────────────────── */

int ove_recursive_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	if (mtx == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (nxrmutex_init(&storage->rmtx) < 0) {
		return OVE_ERR_NO_MEMORY;
	}
	*mtx = storage;
	return OVE_OK;
}

void ove_recursive_mutex_deinit(ove_mutex_t mtx)
{
	if (mtx != NULL) {
		nxrmutex_destroy(&mtx->rmtx);
	}
}

/* ─── Recursive Mutex _create / _destroy ─────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_recursive_mutex_create(ove_mutex_t *mtx)
{
	int ret = ove_check_param(mtx);
	if (ret)
		return ret;

	struct ove_mutex *m = OVE_BACKEND_MALLOC(sizeof(*m));
	if (m == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	ret = ove_recursive_mutex_init(mtx, m);
	if (ret != OVE_OK) {
		/* If nxrmutex_init succeeded before init failed elsewhere, tear it down */
		nxrmutex_destroy(&m->rmtx);
		OVE_BACKEND_FREE(m);
	}
	return ret;
}

void ove_recursive_mutex_destroy(ove_mutex_t mtx)
{
	if (mtx != NULL) {
		ove_recursive_mutex_deinit(mtx);
		OVE_BACKEND_FREE(mtx);
	}
}
#endif /* OVE_HEAP_SYNC */

int ove_recursive_mutex_lock(ove_mutex_t mtx, uint64_t timeout_ns)
{
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_ENTER, mtx);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);

	int ret;
	if (ove_timeout_is_forever(timeout_ns)) {
		ret = nxrmutex_lock(&mtx->rmtx);
	} else {
		ret = nxrmutex_ticklock(&mtx->rmtx, ove_ns_to_ticks(timeout_ns));
	}

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_EXIT, mtx);
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_recursive_mutex_unlock(ove_mutex_t mtx)
{
	nxrmutex_unlock(&mtx->rmtx);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_POST, mtx);
}

/* ─── Semaphore _init / _deinit ──────────────────────────────────────── */

int ove_sem_init(ove_sem_t *sem, ove_sem_storage_t *storage, unsigned int initial, unsigned int max)
{
	if (sem == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	(void)max;
	nxsem_init(&storage->sem, 0, initial);
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	*sem = storage;
	return OVE_OK;
}

void ove_sem_deinit(ove_sem_t sem)
{
	if (sem != NULL) {
		nxsem_destroy(&sem->sem);
	}
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

	int ret;
	if (ove_timeout_is_forever(timeout_ns)) {
		ret = nxsem_wait_uninterruptible(&sem->sem);
	} else {
		ret = nxsem_tickwait_uninterruptible(&sem->sem, ove_ns_to_ticks(timeout_ns));
	}

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_WAIT_EXIT, sem);
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_sem_give(ove_sem_t sem)
{
	nxsem_post(&sem->sem);
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
	nxsem_init(&storage->sem, 0, 0);
	*evt = storage;
	return OVE_OK;
}

void ove_event_deinit(ove_event_t evt)
{
	if (evt != NULL) {
		nxsem_destroy(&evt->sem);
	}
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

	int ret;
	if (ove_timeout_is_forever(timeout_ns)) {
		ret = nxsem_wait_uninterruptible(&evt->sem);
	} else {
		ret = nxsem_tickwait_uninterruptible(&evt->sem, ove_ns_to_ticks(timeout_ns));
	}

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_EXIT, evt);
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

static void event_signal(ove_event_t evt)
{
	/* An ove_event is a binary auto-reset event, not a counting semaphore.
	 * Keep NuttX's backing semaphore at one token just like Zephyr's k_sem
	 * max=1 and FreeRTOS's explicit latch. The critical section makes the
	 * read-and-post atomic against both task and ISR signalers. */
	irqstate_t flags = enter_critical_section();
	int value;
	if (nxsem_get_value(&evt->sem, &value) == 0 && value < 1)
		(void)nxsem_post(&evt->sem);
	leave_critical_section(flags);
}

void ove_event_signal(ove_event_t evt)
{
	event_signal(evt);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_POST, evt);
}

void ove_event_signal_from_isr(ove_event_t evt)
{
	event_signal(evt);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_POST, evt);
}

/* ─── Condvar _init / _deinit ────────────────────────────────────────── */

int ove_condvar_init(ove_condvar_t *cv, ove_condvar_storage_t *storage)
{
	if (cv == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	nxsem_init(&storage->waiter, 0, 0);
	nxmutex_init(&storage->guard);
	storage->nwaiters = 0;
	*cv = storage;
	return OVE_OK;
}

void ove_condvar_deinit(ove_condvar_t cv)
{
	if (cv != NULL) {
		nxsem_destroy(&cv->waiter);
		nxmutex_destroy(&cv->guard);
	}
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
	int ret;

	while (nxmutex_lock(&cv->guard) == -EINTR)
		;
	cv->nwaiters++;
	nxmutex_unlock(&cv->guard);

	nxmutex_unlock(&mtx->mtx);

	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_ENTER, cv);
	ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);

	if (ove_timeout_is_forever(timeout_ns)) {
		ret = nxsem_wait_uninterruptible(&cv->waiter);
	} else {
		ret = nxsem_tickwait_uninterruptible(&cv->waiter, ove_ns_to_ticks(timeout_ns));
	}

	ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_EXIT, cv);

	/* Re-acquire caller's mutex */
	while (nxmutex_lock(&mtx->mtx) == -EINTR)
		;

	while (nxmutex_lock(&cv->guard) == -EINTR)
		;
	cv->nwaiters--;
	nxmutex_unlock(&cv->guard);

	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_condvar_signal(ove_condvar_t cv)
{
	while (nxmutex_lock(&cv->guard) == -EINTR)
		;
	if (cv->nwaiters > 0) {
		nxsem_post(&cv->waiter);
	}
	nxmutex_unlock(&cv->guard);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_POST, cv);
}

void ove_condvar_broadcast(ove_condvar_t cv)
{
	while (nxmutex_lock(&cv->guard) == -EINTR)
		;
	for (int i = 0; i < cv->nwaiters; i++) {
		nxsem_post(&cv->waiter);
	}
	nxmutex_unlock(&cv->guard);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_POST, cv);
}
