/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/workqueue.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

#include <string.h>
static UBaseType_t map_priority(ove_prio_t prio)
{
	return tskIDLE_PRIORITY + (UBaseType_t)prio;
}

static void wq_thread(void *arg)
{
	struct ove_workqueue *wq = (struct ove_workqueue *)arg;
	struct ove_work *work;

	while (wq->running) {
		if (xQueueReceive(wq->queue, &work, portMAX_DELAY) == pdPASS) {
			if (work == NULL) {
				break; /* poison pill — shutdown */
			}
			/* Pulled from the queue — no longer pending. */
			__atomic_store_n(&work->pending, 0, __ATOMIC_RELEASE);
			__atomic_store_n(&work->in_progress, 1, __ATOMIC_RELEASE);
			if (work->handler != NULL) {
				work->handler(work);
			}
			__atomic_store_n(&work->in_progress, 0, __ATOMIC_RELEASE);
			/* Unconditional give: cancel/free's wait loop drains
			 * stale tokens before re-checking in_progress, so an
			 * extra give from a prior iteration is harmless. */
			xSemaphoreGive(work->completion_sem);
		}
	}
	/* Signal that worker has finished processing */
	xSemaphoreGive(wq->done_sem);
	vTaskSuspend(NULL);
}

/* Wait until the worker has fully released this work item.
 *
 * The loop pattern handles three cases without races:
 *   1. Worker not yet pulled the work — in_progress==0, immediate exit.
 *   2. Worker mid-handler — Take blocks until worker's give; loop
 *      re-checks because that give might be from a prior iteration.
 *   3. Worker just finished — in_progress==0 but a stale give may
 *      sit on the sem; subsequent Take drains it and we exit.
 */
static void wait_for_completion(struct ove_work *w)
{
	while (__atomic_load_n(&w->in_progress, __ATOMIC_ACQUIRE)) {
		xSemaphoreTake(w->completion_sem, portMAX_DELAY);
	}
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_workqueue_init(ove_workqueue_t *wq, ove_workqueue_storage_t *storage, const char *name,
		       ove_prio_t priority, size_t stack_size, void *stack)
{
	if (wq == NULL || storage == NULL || stack == NULL)
		return OVE_ERR_INVALID_PARAM;

	storage->queue = xQueueCreateStatic(OVE_WQ_QUEUE_DEPTH, sizeof(struct ove_work *),
					    storage->queue_storage, &storage->static_queue);
	storage->done_sem = xSemaphoreCreateBinaryStatic(&storage->static_done_sem);
	storage->running = 1;

	uint32_t stack_depth = stack_size / sizeof(StackType_t);
	if (stack_depth < configMINIMAL_STACK_SIZE)
		stack_depth = configMINIMAL_STACK_SIZE;

	storage->task = xTaskCreateStatic(wq_thread, name ? name : "ove_wq", stack_depth, storage,
					  map_priority(priority), (StackType_t *)stack,
					  &storage->static_task);

	*wq = storage;
	return OVE_OK;
}

void ove_workqueue_deinit(ove_workqueue_t wq)
{
	if (wq != NULL) {
		struct ove_work *poison = NULL;
		wq->running = 0;
		xQueueSend(wq->queue, &poison, portMAX_DELAY);
		xSemaphoreTake(wq->done_sem, portMAX_DELAY);
		vTaskDelete(wq->task);
	}
}

int ove_work_init_static(ove_work_t *work, ove_work_storage_t *storage, ove_work_fn handler)
{
	if (work == NULL || storage == NULL || handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->handler = handler;
	storage->delay_timer = NULL;
	storage->target_wq = NULL;
	storage->in_progress = 0;
	storage->pending = 0;
	storage->completion_sem = xSemaphoreCreateBinaryStatic(&storage->static_completion_sem);

	*work = storage;
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_WORKQUEUE
int ove_workqueue_create(ove_workqueue_t *wq, const char *name, ove_prio_t priority,
			 size_t stack_size)
{
	struct ove_workqueue *fwq;
	BaseType_t ret;
	uint32_t stack_depth;

	if (wq == NULL)
		return OVE_ERR_INVALID_PARAM;

	fwq = OVE_BACKEND_MALLOC(sizeof(*fwq));
	if (fwq == NULL)
		return OVE_ERR_NO_MEMORY;

	fwq->queue = xQueueCreateStatic(OVE_WQ_QUEUE_DEPTH, sizeof(struct ove_work *),
					fwq->queue_storage, &fwq->static_queue);
	fwq->done_sem = xSemaphoreCreateBinaryStatic(&fwq->static_done_sem);
	fwq->running = 1;

	stack_depth = stack_size / sizeof(StackType_t);
	if (stack_depth < configMINIMAL_STACK_SIZE)
		stack_depth = configMINIMAL_STACK_SIZE;

	ret = xTaskCreate(wq_thread, name ? name : "ove_wq", stack_depth, fwq,
			  map_priority(priority), &fwq->task);
	if (ret != pdPASS) {
		OVE_BACKEND_FREE(fwq);
		return OVE_ERR_NO_MEMORY;
	}

	*wq = fwq;
	return OVE_OK;
}

void ove_workqueue_destroy(ove_workqueue_t wq)
{
	if (wq != NULL) {
		ove_workqueue_deinit(wq);
		OVE_BACKEND_FREE(wq);
	}
}

#endif /* OVE_HEAP_WORKQUEUE */

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_work_init(ove_work_t *work, ove_work_fn handler)
{
	struct ove_work *fw;

	if (work == NULL || handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	fw = OVE_BACKEND_MALLOC(sizeof(*fw));
	if (fw == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	fw->handler = handler;
	fw->delay_timer = NULL;
	fw->target_wq = NULL;
	fw->in_progress = 0;
	fw->pending = 0;
	fw->completion_sem = xSemaphoreCreateBinaryStatic(&fw->static_completion_sem);

	*work = fw;
	return OVE_OK;
}

void ove_work_free(ove_work_t work)
{
	if (work != NULL) {
		/* Wait for any in-flight handler to finish before reclaiming
		 * the struct — closes the UAF window. */
		wait_for_completion(work);
		if (work->delay_timer != NULL) {
			xTimerDelete(work->delay_timer, portMAX_DELAY);
		}
		OVE_BACKEND_FREE(work);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_work_submit(ove_workqueue_t wq, ove_work_t work)
{
	/* Mark pending before enqueue so the worker (which clears it on
	 * dequeue) and a concurrent cancel see a consistent value. */
	__atomic_store_n(&work->pending, 1, __ATOMIC_RELEASE);
	if (xQueueSend(wq->queue, &work, 0) != pdPASS) {
		__atomic_store_n(&work->pending, 0, __ATOMIC_RELEASE);
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

static void delay_timer_cb(TimerHandle_t xTimer)
{
	struct ove_work *fw = pvTimerGetTimerID(xTimer);
	ove_workqueue_t fwq = fw->target_wq;

	xQueueSend(fwq->queue, &fw, 0);
}

int ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t work, uint32_t delay_ms)
{
	work->target_wq = wq;

	if (work->delay_timer == NULL) {
		work->delay_timer = xTimerCreateStatic("wq_delay", pdMS_TO_TICKS(delay_ms), pdFALSE,
						       (void *)work, delay_timer_cb,
						       &work->static_timer);
		if (work->delay_timer == NULL) {
			return OVE_ERR_NO_MEMORY;
		}
	} else {
		xTimerChangePeriod(work->delay_timer, pdMS_TO_TICKS(delay_ms), portMAX_DELAY);
	}

	if (xTimerStart(work->delay_timer, portMAX_DELAY) != pdPASS) {
		return OVE_ERR_TIMEOUT;
	}
	/* Scheduled — pending until the timer fires and the worker dequeues it. */
	__atomic_store_n(&work->pending, 1, __ATOMIC_RELEASE);
	return OVE_OK;
}

int ove_work_cancel(ove_work_t work)
{
	if (work == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int was_pending = __atomic_exchange_n(&work->pending, 0, __ATOMIC_ACQ_REL);
	if (work->delay_timer != NULL) {
		xTimerStop(work->delay_timer, portMAX_DELAY);
	}
	/* Wait for any in-flight handler to finish so the caller may
	 * safely free the struct after cancel returns. */
	wait_for_completion(work);
	/* Contract: OVE_ERR_INVAL when the item was not pending. */
	return was_pending ? OVE_OK : OVE_ERR_INVAL;
}
