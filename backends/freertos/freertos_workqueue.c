/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/workqueue.h"
#include "ove/storage.h"
#include "ove_freertos_priority.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

#include <string.h>

enum {
	WORK_IDLE,
	WORK_DELAYED,
	WORK_QUEUED,
	WORK_RUNNING,
	WORK_CANCELLED,
};

static void work_complete(struct ove_work *work)
{
	work->target_wq = NULL;
	__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
	xSemaphoreGive(work->completion_sem);
}

static void wq_thread(void *arg)
{
	struct ove_workqueue *wq = (struct ove_workqueue *)arg;
	struct ove_work *work;

	while (1) {
		if (xQueueReceive(wq->queue, &work, portMAX_DELAY) == pdPASS) {
			if (work == NULL) {
				break; /* poison pill — shutdown */
			}
			int expected = WORK_QUEUED;
			if (wq->running &&
			    __atomic_compare_exchange_n(&work->state, &expected, WORK_RUNNING, 0,
							__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
			    work->handler != NULL) {
				work->handler(work);
			}
			work_complete(work);
		}
	}
	/* Signal that worker has finished processing */
	xSemaphoreGive(wq->done_sem);
	vTaskSuspend(NULL);
}

/* Wait until the worker has fully released this work item.
 *
 * The loop pattern handles three cases without races:
 *   1. Worker has not yet pulled the work — Take blocks for its ack.
 *   2. Worker is in the handler — Take blocks until completion.
 *   3. Worker just finished — idle is observed and a stale give may
 *      sit on the sem; subsequent Take drains it and we exit.
 */
static void wait_for_completion(struct ove_work *w)
{
	while (__atomic_load_n(&w->state, __ATOMIC_ACQUIRE) != WORK_IDLE) {
		xSemaphoreTake(w->completion_sem, portMAX_DELAY);
	}
}

static void work_timer_drain_done(void *param1, uint32_t param2)
{
	(void)param2;
	xSemaphoreGive((SemaphoreHandle_t)param1);
}

static void drain_timer_daemon(void)
{
	StaticSemaphore_t sem_buf;
	SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&sem_buf);
	if (done != NULL &&
	    xTimerPendFunctionCall(work_timer_drain_done, done, 0, portMAX_DELAY) == pdPASS)
		xSemaphoreTake(done, portMAX_DELAY);
}

static TickType_t work_delay_ticks(uint32_t delay_ms)
{
	TickType_t ticks = pdMS_TO_TICKS(delay_ms);
	return ticks > 0 ? ticks : 1;
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_workqueue_init(ove_workqueue_t *wq, ove_workqueue_storage_t *storage, const char *name,
		       ove_prio_t priority, size_t stack_size, void *stack)
{
	if (wq == NULL || storage == NULL)
		return OVE_ERR_INVALID_PARAM;
#ifndef OVE_HEAP_WORKQUEUE
	if (stack == NULL)
		return OVE_ERR_INVALID_PARAM;
#endif

	storage->queue = xQueueCreateStatic(OVE_WQ_QUEUE_DEPTH, sizeof(struct ove_work *),
					    storage->queue_storage, &storage->static_queue);
	storage->done_sem = xSemaphoreCreateBinaryStatic(&storage->static_done_sem);
	storage->running = 1;

	uint32_t stack_depth = stack_size / sizeof(StackType_t);
	if (stack_depth < configMINIMAL_STACK_SIZE)
		stack_depth = configMINIMAL_STACK_SIZE;

#ifdef OVE_HEAP_WORKQUEUE
	if (stack == NULL) {
		if (xTaskCreate(wq_thread, name ? name : "ove_wq", stack_depth, storage,
				ove_freertos_map_priority(priority), &storage->task) != pdPASS)
			return OVE_ERR_NO_MEMORY;
	} else
#endif
	{
		storage->task = xTaskCreateStatic(wq_thread, name ? name : "ove_wq", stack_depth,
						  storage, ove_freertos_map_priority(priority),
						  (StackType_t *)stack, &storage->static_task);
		if (storage->task == NULL)
			return OVE_ERR_NO_MEMORY;
	}

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
	storage->state = WORK_IDLE;
	storage->completion_sem = xSemaphoreCreateBinaryStatic(&storage->static_completion_sem);

	*work = storage;
	return OVE_OK;
}

void ove_work_deinit(ove_work_t work)
{
	if (work == NULL)
		return;
	(void)ove_work_cancel(work);
	if (work->delay_timer != NULL) {
		xTimerDelete(work->delay_timer, portMAX_DELAY);
		drain_timer_daemon();
		work->delay_timer = NULL;
	}
	work->handler = NULL;
}

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_work_submit(ove_workqueue_t wq, ove_work_t work)
{
	if (wq == NULL || work == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (!wq->running)
		return OVE_ERR_BUSY;
	int expected = WORK_IDLE;
	if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_QUEUED, 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		return OVE_ERR_BUSY;
	work->target_wq = wq;
	if (xQueueSend(wq->queue, &work, 0) != pdPASS) {
		work->target_wq = NULL;
		__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

static void delay_timer_cb(TimerHandle_t xTimer)
{
	struct ove_work *fw = pvTimerGetTimerID(xTimer);
	ove_workqueue_t fwq = fw->target_wq;
	int expected = WORK_DELAYED;
	if (!__atomic_compare_exchange_n(&fw->state, &expected, WORK_QUEUED, 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		return;
	if (fwq == NULL || !fwq->running || xQueueSend(fwq->queue, &fw, 0) != pdPASS)
		work_complete(fw);
}

int ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t work, uint32_t delay_ms)
{
	if (wq == NULL || work == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (!wq->running)
		return OVE_ERR_BUSY;
	int expected = WORK_IDLE;
	if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_DELAYED, 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		return OVE_ERR_BUSY;
	work->target_wq = wq;

	if (work->delay_timer == NULL) {
		work->delay_timer = xTimerCreateStatic("wq_delay", work_delay_ticks(delay_ms),
						       pdFALSE, (void *)work, delay_timer_cb,
						       &work->static_timer);
		if (work->delay_timer == NULL) {
			work->target_wq = NULL;
			__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
			return OVE_ERR_NO_MEMORY;
		}
	} else {
		if (xTimerChangePeriod(work->delay_timer, work_delay_ticks(delay_ms),
				       portMAX_DELAY) != pdPASS) {
			work->target_wq = NULL;
			__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
			return OVE_ERR_TIMEOUT;
		}
		return OVE_OK;
	}

	if (xTimerStart(work->delay_timer, portMAX_DELAY) != pdPASS) {
		work->target_wq = NULL;
		__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_work_cancel(ove_work_t work)
{
	if (work == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	for (;;) {
		int state = __atomic_load_n(&work->state, __ATOMIC_ACQUIRE);
		if (state == WORK_IDLE)
			return OVE_ERR_INVAL;
		if (state == WORK_DELAYED) {
			int expected = WORK_DELAYED;
			if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_CANCELLED, 0,
							 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
				continue;
			xTimerStop(work->delay_timer, portMAX_DELAY);
			drain_timer_daemon();
			work_complete(work);
			return OVE_OK;
		}
		if (state == WORK_QUEUED) {
			int expected = WORK_QUEUED;
			if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_CANCELLED, 0,
							 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
				continue;
			wait_for_completion(work);
			return OVE_OK;
		}
		/* Running, or another caller already requested cancellation. */
		wait_for_completion(work);
		return OVE_ERR_INVAL;
	}
}
