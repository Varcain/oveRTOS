/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/sync.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static TickType_t ms_to_ticks(uint32_t ms)
{
	if (ove_timeout_is_forever(ms)) {
		return portMAX_DELAY;
	}
	return pdMS_TO_TICKS(ms);
}

/* ─── Mutex _init / _deinit ──────────────────────────────────────────── */

int ove_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage)
{
	if (mtx == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
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
	if (ret) return ret;

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

int ove_mutex_lock(ove_mutex_t mtx, uint32_t timeout_ms)
{
	if (xSemaphoreTake(mtx->sem,
			   ms_to_ticks(timeout_ms)) == pdTRUE) {
		return OVE_OK;
	}
	return OVE_ERR_TIMEOUT;
}

void ove_mutex_unlock(ove_mutex_t mtx)
{
	xSemaphoreGive(mtx->sem);
}

/* ─── Semaphore _init / _deinit ──────────────────────────────────────── */

int ove_sem_init(ove_sem_t *sem, ove_sem_storage_t *storage,
		     unsigned int initial, unsigned int max)
{
	if (sem == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	storage->sem = xSemaphoreCreateCountingStatic(max, initial,
						      &storage->static_sem);
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
int ove_sem_create(ove_sem_t *sem, unsigned int initial,
			       unsigned int max)
{
	int ret = ove_check_param(sem);
	if (ret) return ret;

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

int ove_sem_take(ove_sem_t sem, uint32_t timeout_ms)
{
	if (xSemaphoreTake(sem->sem,
			   ms_to_ticks(timeout_ms)) == pdTRUE) {
		return OVE_OK;
	}
	return OVE_ERR_TIMEOUT;
}

void ove_sem_give(ove_sem_t sem)
{
	xSemaphoreGive(sem->sem);
}

/* ─── Event _init / _deinit ──────────────────────────────────────────── */

int ove_event_init(ove_event_t *evt, ove_event_storage_t *storage)
{
	if (evt == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	storage->sem = xSemaphoreCreateBinaryStatic(&storage->static_sem);
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
	if (ret) return ret;

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

int ove_event_wait(ove_event_t evt, uint32_t timeout_ms)
{
	if (xSemaphoreTake(evt->sem,
			   ms_to_ticks(timeout_ms)) == pdTRUE) {
		return OVE_OK;
	}
	return OVE_ERR_TIMEOUT;
}

void ove_event_signal(ove_event_t evt)
{
	xSemaphoreGive(evt->sem);
}

void ove_event_signal_from_isr(ove_event_t evt)
{
	BaseType_t yield = pdFALSE;
	xSemaphoreGiveFromISR(evt->sem, &yield);
	portYIELD_FROM_ISR(yield);
}

/* ─── Recursive Mutex _init ──────────────────────────────────────────── */

int ove_recursive_mutex_init(ove_mutex_t *mtx,
				 ove_mutex_storage_t *storage)
{
	if (mtx == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	storage->sem = xSemaphoreCreateRecursiveMutexStatic(
		&storage->static_sem);
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

int ove_recursive_mutex_lock(ove_mutex_t mtx,
					 uint32_t timeout_ms)
{
	if (xSemaphoreTakeRecursive(mtx->sem,
				    ms_to_ticks(timeout_ms)) == pdTRUE) {
		return OVE_OK;
	}
	return OVE_ERR_TIMEOUT;
}

void ove_recursive_mutex_unlock(ove_mutex_t mtx)
{
	xSemaphoreGiveRecursive(mtx->sem);
}

/* ─── Condition Variable ─────────────────────────────────────────────── */

struct condvar_waiter {
	TaskHandle_t task;
	struct condvar_waiter *next;
};

/* ─── Condvar _init / _deinit ────────────────────────────────────────── */

int ove_condvar_init(ove_condvar_t *cv,
			 ove_condvar_storage_t *storage)
{
	if (cv == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	storage->guard = xSemaphoreCreateMutexStatic(&storage->static_guard);
	storage->head = NULL;
	*cv = storage;
	return OVE_OK;
}

void ove_condvar_deinit(ove_condvar_t cv)
{
	if (cv != NULL && cv->guard != NULL) {
		vSemaphoreDelete(cv->guard);
		cv->guard = NULL;
	}
}

/* ─── Condvar _create / _destroy ─────────────────────────────────────── */

#ifdef OVE_HEAP_SYNC
int ove_condvar_create(ove_condvar_t *cv)
{
	int ret = ove_check_param(cv);
	if (ret) return ret;

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
	if (cv == NULL) {
		return;
	}
	ove_condvar_deinit(cv);
	OVE_BACKEND_FREE(cv);
}
#endif /* OVE_HEAP_SYNC */

int ove_condvar_wait(ove_condvar_t cv, ove_mutex_t mtx,
				 uint32_t timeout_ms)
{
	struct condvar_waiter self;

	self.task = xTaskGetCurrentTaskHandle();

	/* Register in wait list before releasing mutex */
	xSemaphoreTake(cv->guard, portMAX_DELAY);
	self.next = cv->head;
	cv->head = &self;
	xSemaphoreGive(cv->guard);

	/* Release the caller's mutex */
	xSemaphoreGive(mtx->sem);

	/* Wait for direct notification from signal/broadcast */
	uint32_t got = ulTaskNotifyTake(pdTRUE, ms_to_ticks(timeout_ms));

	if (got == 0) {
		/* Timeout — remove ourselves from the list if still there */
		xSemaphoreTake(cv->guard, portMAX_DELAY);
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
		xSemaphoreGive(cv->guard);

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
	xSemaphoreTake(cv->guard, portMAX_DELAY);
	if (cv->head != NULL) {
		TaskHandle_t task = cv->head->task;
		cv->head = cv->head->next;
		xTaskNotifyGive(task);
	}
	xSemaphoreGive(cv->guard);
}

void ove_condvar_broadcast(ove_condvar_t cv)
{
	xSemaphoreTake(cv->guard, portMAX_DELAY);
	struct condvar_waiter *p = cv->head;
	cv->head = NULL;
	while (p) {
		xTaskNotifyGive(p->task);
		p = p->next;
	}
	xSemaphoreGive(cv->guard);
}
