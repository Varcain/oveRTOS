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
#include "ove_zephyr_priority.h"
#include <zephyr/kernel.h>

static void zephyr_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct ove_work *zw = CONTAINER_OF(dwork, struct ove_work, dwork);
	if (zw->handler != NULL) {
		zw->handler(zw);
	}
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_workqueue_init(ove_workqueue_t *wq, ove_workqueue_storage_t *storage, const char *name,
		       ove_prio_t priority, size_t stack_size, void *stack)
{
	if (wq == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (stack_size == 0) {
		stack_size = 4096;
	}

	if (stack != NULL) {
		storage->stack = (k_thread_stack_t *)stack;
		storage->heap_stack = 0;
	} else {
#ifdef CONFIG_OVE_ZERO_HEAP
		/* Zero-heap mode: no kernel stack pool, NULL stack is a misuse. */
		return OVE_ERR_NO_MEMORY;
#else
		storage->stack = k_thread_stack_alloc(stack_size, 0);
		if (storage->stack == NULL) {
			return OVE_ERR_NO_MEMORY;
		}
		storage->heap_stack = 1;
#endif
	}
	storage->stack_size = stack_size;

	k_work_queue_start(&storage->work_q, storage->stack, stack_size,
			   ove_zephyr_map_priority(priority), NULL);

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
		if (wq->heap_stack && wq->stack != NULL) {
			k_thread_stack_free(wq->stack);
			wq->stack = NULL;
			wq->heap_stack = 0;
		}
	}
}

int ove_work_init_static(ove_work_t *work, ove_work_storage_t *storage, ove_work_fn handler)
{
	if (work == NULL || storage == NULL || handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->handler = handler;
	k_work_init_delayable(&storage->dwork, zephyr_work_handler);

	*work = storage;
	return OVE_OK;
}

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
	if (work != NULL) {
		/* Cancel + wait for any in-flight handler before reclaiming
		 * the struct.  Closes the use-after-free window where the
		 * worker is still inside zephyr_work_handler when the caller
		 * frees w.  Zephyr's k_work_cancel_delayable_sync is the
		 * right primitive — must not be called from ISR or from the
		 * workqueue thread itself, but ove_work_free is documented
		 * as task-context only. */
		struct k_work_sync sync;
		(void)k_work_cancel_delayable_sync(&work->dwork, &sync);
		OVE_BACKEND_FREE(work);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_work_submit(ove_workqueue_t wq, ove_work_t work)
{
	int ret = k_work_schedule_for_queue(&wq->work_q, &work->dwork, K_NO_WAIT);
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

int ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t work, uint32_t delay_ms)
{
	int ret = k_work_schedule_for_queue(&wq->work_q, &work->dwork, K_MSEC(delay_ms));
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

int ove_work_cancel(ove_work_t work)
{
	if (work == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* Sync variant: stops the delay timer AND waits for any running
	 * handler to complete.  After this returns the caller may safely
	 * free the work struct.  The return is true when the work was still
	 * pending/running, false when there was nothing to cancel. */
	struct k_work_sync sync;
	bool was_active = k_work_cancel_delayable_sync(&work->dwork, &sync);
	/* Contract: OVE_ERR_INVAL when the item was not pending. */
	return was_active ? OVE_OK : OVE_ERR_INVAL;
}
