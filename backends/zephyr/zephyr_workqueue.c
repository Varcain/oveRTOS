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
#include <zephyr/kernel.h>

static int map_priority(ove_prio_t prio)
{
	switch (prio) {
	case OVE_PRIO_IDLE:         return 14;
	case OVE_PRIO_LOW:          return 12;
	case OVE_PRIO_BELOW_NORMAL: return 10;
	case OVE_PRIO_NORMAL:       return 8;
	case OVE_PRIO_ABOVE_NORMAL: return 6;
	case OVE_PRIO_HIGH:         return 4;
	case OVE_PRIO_REALTIME:     return 2;
	case OVE_PRIO_CRITICAL:     return 1;
	default:                        return 8;
	}
}

static void zephyr_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct ove_work *zw = CONTAINER_OF(dwork, struct ove_work,
					      dwork);
	if (zw->handler != NULL) {
		zw->handler(zw);
	}
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_workqueue_init(ove_workqueue_t *wq,
			   ove_workqueue_storage_t *storage,
			   const char *name, ove_prio_t priority,
			   size_t stack_size, void *stack)
{
	if (wq == NULL || storage == NULL || stack == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (stack_size == 0) {
		stack_size = 4096;
	}

	storage->stack = (k_thread_stack_t *)stack;
	storage->stack_size = stack_size;

	k_work_queue_start(&storage->work_q, storage->stack, stack_size,
			   map_priority(priority), NULL);

	if (name != NULL) {
		k_thread_name_set(&storage->work_q.thread, name);
	}

	*wq = storage;
	return OVE_OK;
}

void ove_workqueue_deinit(ove_workqueue_t wq)
{
	if (wq != NULL) {
		k_work_queue_drain(&wq->work_q, false);
		k_thread_abort(&wq->work_q.thread);
	}
}

int ove_work_init_static(ove_work_t *work,
			     ove_work_storage_t *storage,
			     ove_work_fn handler)
{
	if (work == NULL || storage == NULL || handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->handler = handler;
	k_work_init_delayable(&storage->dwork, zephyr_work_handler);

	*work = storage;
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_WORKQUEUE
int ove_workqueue_create(ove_workqueue_t *wq, const char *name,
			    ove_prio_t priority, size_t stack_size)
{
	struct ove_workqueue *zwq;

	if (wq == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (stack_size == 0) {
		stack_size = 4096;
	}

	zwq = OVE_BACKEND_MALLOC(sizeof(*zwq));
	if (zwq == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	zwq->stack = k_thread_stack_alloc(stack_size, 0);
	if (zwq->stack == NULL) {
		OVE_BACKEND_FREE(zwq);
		return OVE_ERR_NO_MEMORY;
	}

	zwq->stack_size = stack_size;

	k_work_queue_start(&zwq->work_q, zwq->stack, stack_size,
			   map_priority(priority), NULL);

	if (name != NULL) {
		k_thread_name_set(&zwq->work_q.thread, name);
	}

	*wq = zwq;
	return OVE_OK;
}

void ove_workqueue_destroy(ove_workqueue_t wq)
{
	if (wq != NULL) {
		ove_workqueue_deinit(wq);
		k_thread_stack_free(wq->stack);
		OVE_BACKEND_FREE(wq);
	}
}
#endif /* OVE_HEAP_WORKQUEUE */

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_work_init(ove_work_t *work, ove_work_fn handler)
{
	struct ove_work *zw;

	if (work == NULL || handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	zw = OVE_BACKEND_MALLOC(sizeof(*zw));
	if (zw == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	zw->handler = handler;
	k_work_init_delayable(&zw->dwork, zephyr_work_handler);

	*work = zw;
	return OVE_OK;
}

void ove_work_free(ove_work_t work)
{
	OVE_BACKEND_FREE(work);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_work_submit(ove_workqueue_t wq, ove_work_t work)
{
	int ret = k_work_schedule_for_queue(&wq->work_q, &work->dwork,
					    K_NO_WAIT);
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

int ove_work_submit_delayed(ove_workqueue_t wq,
				      ove_work_t work,
				      uint32_t delay_ms)
{
	int ret = k_work_schedule_for_queue(&wq->work_q, &work->dwork,
					    K_MSEC(delay_ms));
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

int ove_work_cancel(ove_work_t work)
{
	k_work_cancel_delayable(&work->dwork);
	return OVE_OK;
}
