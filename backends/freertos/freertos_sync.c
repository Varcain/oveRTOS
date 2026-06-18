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
	__atomic_store_n(&storage->signaled, 0u, __ATOMIC_RELAXED);
	__atomic_store_n(&storage->waiter, (TaskHandle_t)NULL, __ATOMIC_RELAXED);
	*evt = storage;
	return OVE_OK;
}

void ove_event_deinit(ove_event_t evt)
{
	(void)evt; /* no kernel object to release */
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
	return ove_event_init(evt, w); /* cannot fail — no kernel object */
}

void ove_event_destroy(ove_event_t evt)
{
	if (evt != NULL) {
		OVE_BACKEND_FREE(evt);
	}
}
#endif /* OVE_HEAP_SYNC */

int ove_event_wait(ove_event_t evt, uint64_t timeout_ns)
{
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_ENTER, evt);

	/* Drop any stale notification left in this task's single slot (e.g.
	 * from another event whose signaller raced our deregister) so it can't
	 * satisfy this wait spuriously. */
	(void)ulTaskNotifyTake(pdTRUE, 0);

	const int forever = (timeout_ns == OVE_WAIT_FOREVER);
	const TickType_t deadline =
		xTaskGetTickCount() + (forever ? 0 : ove_ns_to_ticks(timeout_ns));
	int ret = OVE_ERR_TIMEOUT;

	/* Register as the (single) waiter for the signaller to notify. */
	__atomic_store_n(&evt->waiter, xTaskGetCurrentTaskHandle(), __ATOMIC_SEQ_CST);
	for (;;) {
		/* The latch is the per-event source of truth; a task
		 * notification is only a wakeup hint (per-task, shared across
		 * every event this thread waits on).  Checking the latch first
		 * also closes the signal-between-register-and-block race. */
		if (__atomic_exchange_n(&evt->signaled, 0u, __ATOMIC_SEQ_CST) != 0u) {
			ret = OVE_OK;
			break;
		}

		TickType_t wait_ticks;
		if (forever) {
			wait_ticks = portMAX_DELAY;
		} else {
			TickType_t now = xTaskGetTickCount();
			if ((int32_t)(deadline - now) <= 0) {
				ret = OVE_ERR_TIMEOUT;
				break;
			}
			wait_ticks = deadline - now;
		}

		uint32_t got = ulTaskNotifyTake(pdTRUE, wait_ticks);

		/* Re-check the latch regardless of why we woke — a signal that
		 * raced the timeout still set it, and `got` alone can't tell our
		 * event's signal from a stale cross-event notification. */
		if (__atomic_exchange_n(&evt->signaled, 0u, __ATOMIC_SEQ_CST) != 0u) {
			ret = OVE_OK;
			break;
		}
		if (got == 0u) {
			ret = OVE_ERR_TIMEOUT; /* timed out, no latch (forever never hits this) */
			break;
		}
		/* got != 0 but no latch: stale notification from another event —
		 * re-block on the remaining time. */
	}
	__atomic_store_n(&evt->waiter, (TaskHandle_t)NULL, __ATOMIC_SEQ_CST);

	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_WAIT_EXIT, evt);
	return ret;
}

void ove_event_signal(ove_event_t evt)
{
	/* Publish the latch first (level-triggered), then wake the waiter if
	 * one is registered.  SEQ_CST pairs with ove_event_wait's
	 * register-then-recheck so a signal can never be lost. */
	__atomic_store_n(&evt->signaled, 1u, __ATOMIC_SEQ_CST);
	TaskHandle_t w = __atomic_load_n(&evt->waiter, __ATOMIC_SEQ_CST);
	if (w != NULL) {
		xTaskNotifyGive(w);
	}
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_EVENT, OVE_TRACE_ACT_POST, evt);
}

void ove_event_signal_from_isr(ove_event_t evt)
{
	__atomic_store_n(&evt->signaled, 1u, __ATOMIC_SEQ_CST);
	TaskHandle_t w = __atomic_load_n(&evt->waiter, __ATOMIC_SEQ_CST);
	BaseType_t yield = pdFALSE;
	if (w != NULL) {
		vTaskNotifyGiveFromISR(w, &yield);
	}
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
