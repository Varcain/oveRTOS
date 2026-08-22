/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/workqueue.h"
#include "ove/storage.h"
#include "ove_zephyr_priority.h"
#include <zephyr/kernel.h>

enum {
	WORK_IDLE,
	WORK_PENDING,
	WORK_RUNNING,
	WORK_CANCELLED,
};

static void zephyr_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct ove_work *zw = CONTAINER_OF(dwork, struct ove_work, dwork);
	int expected = WORK_PENDING;
	if (__atomic_compare_exchange_n(&zw->state, &expected, WORK_RUNNING, 0, __ATOMIC_ACQ_REL,
					__ATOMIC_ACQUIRE)) {
		if (zw->handler != NULL)
			zw->handler(zw);
		__atomic_store_n(&zw->state, WORK_IDLE, __ATOMIC_RELEASE);
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
	storage->state = WORK_IDLE;
	k_work_init_delayable(&storage->dwork, zephyr_work_handler);

	*work = storage;
	return OVE_OK;
}

void ove_work_deinit(ove_work_t work)
{
	if (work != NULL) {
		(void)ove_work_cancel(work);
		work->handler = NULL;
	}
}

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_work_submit(ove_workqueue_t wq, ove_work_t work)
{
	if (wq == NULL || work == NULL)
		return OVE_ERR_INVALID_PARAM;
	int expected = WORK_IDLE;
	if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_PENDING, 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		return OVE_ERR_BUSY;
	int ret = k_work_schedule_for_queue(&wq->work_q, &work->dwork, K_NO_WAIT);
	if (ret < 0)
		__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

int ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t work, uint32_t delay_ms)
{
	if (wq == NULL || work == NULL)
		return OVE_ERR_INVALID_PARAM;
	int expected = WORK_IDLE;
	if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_PENDING, 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		return OVE_ERR_BUSY;
	int ret = k_work_schedule_for_queue(&wq->work_q, &work->dwork, K_MSEC(delay_ms));
	if (ret < 0)
		__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
	return (ret >= 0) ? OVE_OK : OVE_ERR_TIMEOUT;
}

int ove_work_cancel(ove_work_t work)
{
	if (work == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int result = OVE_ERR_INVAL;
	for (;;) {
		int state = __atomic_load_n(&work->state, __ATOMIC_ACQUIRE);
		if (state == WORK_IDLE)
			return OVE_ERR_INVAL;
		if (state == WORK_PENDING) {
			int expected = WORK_PENDING;
			if (!__atomic_compare_exchange_n(&work->state, &expected, WORK_CANCELLED, 0,
							 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
				continue;
			result = OVE_OK;
		}
		break;
	}
	struct k_work_sync sync;
	(void)k_work_cancel_delayable_sync(&work->dwork, &sync);
	if (result == OVE_OK)
		__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
	return result;
}
