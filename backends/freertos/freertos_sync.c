/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/sync.h"
#include "ove/storage.h"
#include "ove/trace.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
#include "ove_ns_to_ticks.h"
#include "semphr.h"
#include "task.h"

/* ─── Mutex _init / _deinit ──────────────────────────────────────────── */

int ove_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	storage->sem = xSemaphoreCreateMutexStatic(&storage->static_sem);
	*mtx = storage;
	return OVE_OK;
}

void ove_mutex_deinit(ove_mutex_t mtx)
{
	if (mtx != NULL && mtx->sem != NULL) {
		vSemaphoreDelete(mtx->sem);
		mtx->sem = NULL;
	}
}

/* ─── Mutex _create / _destroy ───────────────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_mutex_create(ove_mutex_t *mtx)
{
	int ret = ove_check_param(mtx);
	if (ret)
		return ret;

	struct ove_mutex *w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (w == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	ret = ove_mutex_init(mtx, w);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(w);
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
	BaseType_t r = xSemaphoreTake(mtx->sem, ove_ns_to_ticks(timeout_ns));
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_EXIT, mtx);
	return (r == pdTRUE) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_mutex_unlock(ove_mutex_t mtx)
{
	xSemaphoreGive(mtx->sem);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_POST, mtx);
}

/* ─── Semaphore _init / _deinit ──────────────────────────────────────── */

int ove_sem_init(ove_sem_t *sem, ove_sem_storage_t *storage, unsigned int initial, unsigned int max)
{
	storage->sem = xSemaphoreCreateCountingStatic(max, initial, &storage->static_sem);
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	*sem = storage;
	return OVE_OK;
}

void ove_sem_deinit(ove_sem_t sem)
{
	if (sem != NULL && sem->sem != NULL) {
		vSemaphoreDelete(sem->sem);
		sem->sem = NULL;
	}
}

/* ─── Semaphore _create / _destroy ───────────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_sem_create(ove_sem_t *sem, unsigned int initial, unsigned int max)
{
	int ret = ove_check_param(sem);
	if (ret)
		return ret;

	struct ove_sem *w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (w == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	ret = ove_sem_init(sem, w, initial, max);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(w);
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
	BaseType_t r = xSemaphoreTake(sem->sem, ove_ns_to_ticks(timeout_ns));
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_WAIT_EXIT, sem);
	return (r == pdTRUE) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_sem_give(ove_sem_t sem)
{
	xSemaphoreGive(sem->sem);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_SEM, OVE_TRACE_ACT_POST, sem);
	if (sem->notify_cb != NULL) {
		sem->notify_cb(sem->notify_ud);
	}
}

int ove_sem_set_notify(ove_sem_t sem, ove_notify_cb cb, void *user_data)
{
	/* OVE_NONNULL on the public decl already guarantees sem != NULL. */
	sem->notify_cb = cb;
	sem->notify_ud = user_data;
	return OVE_OK;
}

/* ─── Event _init / _deinit ──────────────────────────────────────────── */

int ove_event_init(ove_event_t *evt, ove_event_storage_t *storage)
{
	storage->sem = xSemaphoreCreateBinaryStatic(&storage->static_sem);
	if (storage->sem == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	*evt = storage;
	return OVE_OK;
}

void ove_event_deinit(ove_event_t evt)
{
	if (evt != NULL && evt->sem != NULL) {
		vSemaphoreDelete(evt->sem);
		evt->sem = NULL;
	}
}

/* ─── Event _create / _destroy ───────────────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_event_create(ove_event_t *evt)
{
	int ret = ove_check_param(evt);
	if (ret)
		return ret;

	struct ove_event *w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (w == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	ret = ove_event_init(evt, w);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(w);
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
	BaseType_t got = xSemaphoreTake(evt->sem, ove_ns_to_ticks(timeout_ns));
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_EXIT, evt);
	return (got == pdTRUE) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_event_signal(ove_event_t evt)
{
	(void)xSemaphoreGive(evt->sem);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_POST, evt);
}

void ove_event_signal_from_isr(ove_event_t evt)
{
	BaseType_t yield = pdFALSE;
	(void)xSemaphoreGiveFromISR(evt->sem, &yield);
	portYIELD_FROM_ISR(yield);
}

/* ─── Recursive Mutex _init ──────────────────────────────────────────── */

int ove_recursive_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	storage->sem = xSemaphoreCreateRecursiveMutexStatic(&storage->static_sem);
	*mtx = storage;
	return OVE_OK;
}

/* ─── Recursive Mutex _create / _destroy ─────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_recursive_mutex_create(ove_mutex_t *mtx)
{
	struct ove_mutex *w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (w == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	int ret = ove_recursive_mutex_init(mtx, w);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(w);
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
	BaseType_t r = xSemaphoreTakeRecursive(mtx->sem, ove_ns_to_ticks(timeout_ns));
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_WAIT_EXIT, mtx);
	return (r == pdTRUE) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_recursive_mutex_unlock(ove_mutex_t mtx)
{
	xSemaphoreGiveRecursive(mtx->sem);
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_MUTEX, OVE_TRACE_ACT_POST, mtx);
}

/* ─── Condition Variable ─────────────────────────────────────────────── */

struct condvar_waiter {
	TaskHandle_t task;
	struct condvar_waiter *next;
};

/* ─── Condvar _init / _deinit ────────────────────────────────────────── */

int ove_condvar_init(ove_condvar_t *cv, ove_condvar_storage_t *storage)
{
	storage->head = NULL;
	*cv = storage;
	return OVE_OK;
}

void ove_condvar_deinit(ove_condvar_t cv)
{
	if (cv != NULL) {
		cv->head = NULL;
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
	OVE_BACKEND_FREE(cv);
}
#endif /* OVE_HEAP_SYNC */

int ove_condvar_wait(ove_condvar_t cv, ove_mutex_t mtx, uint64_t timeout_ns)
{
	struct condvar_waiter self;

	self.task = xTaskGetCurrentTaskHandle();

	/* Register in wait list before releasing mutex.  The waiter list is
	 * a 2-pointer linked-list; updates fit comfortably in a critical
	 * section (canonical FreeRTOS idiom for short list operations) and
	 * avoid a mutex round-trip on the hot path. */
	taskENTER_CRITICAL();
	self.next = cv->head;
	cv->head = &self;
	taskEXIT_CRITICAL();

	/* Release the caller's mutex */
	xSemaphoreGive(mtx->sem);

	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_ENTER, cv);
	/* Wait for direct notification from signal/broadcast */
	uint32_t got = ulTaskNotifyTake(pdTRUE, ove_ns_to_ticks(timeout_ns));
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_WAIT_EXIT, cv);

	if (got == 0) {
		/* Timeout — remove ourselves from the list if still there */
		taskENTER_CRITICAL();
		struct condvar_waiter **pp = &cv->head;
		int found = 0;
		while (*pp) {
			if (*pp == &self) {
				*pp = self.next;
				found = 1;
				break;
			}
			pp = &(*pp)->next;
		}
		taskEXIT_CRITICAL();

		if (!found) {
			/*
			 * Signal/broadcast already removed us and sent a
			 * notification that arrived after our timeout —
			 * consume it and treat as signaled.
			 */
			ulTaskNotifyTake(pdTRUE, 0);
			got = 1;
		}
	}

	/* Re-acquire the caller's mutex */
	xSemaphoreTake(mtx->sem, portMAX_DELAY);

	return (got != 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

void ove_condvar_signal(ove_condvar_t cv)
{
	TaskHandle_t task = NULL;
	taskENTER_CRITICAL();
	if (cv->head != NULL) {
		task = cv->head->task;
		cv->head = cv->head->next;
	}
	taskEXIT_CRITICAL();
	if (task != NULL) {
		xTaskNotifyGive(task);
	}
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_POST, cv);
}

void ove_condvar_broadcast(ove_condvar_t cv)
{
	/* Detach the entire waiter list under the critical section, then
	 * notify each task with interrupts re-enabled.  The notify itself
	 * is a single FreeRTOS call that takes the scheduler lock for a
	 * short window; doing it inside the critical section would
	 * unnecessarily extend the interrupt-masked region. */
	taskENTER_CRITICAL();
	struct condvar_waiter *p = cv->head;
	cv->head = NULL;
	taskEXIT_CRITICAL();
	while (p) {
		xTaskNotifyGive(p->task);
		p = p->next;
	}
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_CV, OVE_TRACE_ACT_POST, cv);
}
