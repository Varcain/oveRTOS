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
#include "ove_nuttx_priority.h"
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
static int wq_task_fn(int argc, char *argv[])
{
	struct ove_workqueue *nwq;
	struct ove_work *work;

	(void)argc;
	nwq = (struct ove_workqueue *)strtoul(argv[1], NULL, 0);

	while (nwq->running) {
		int ret = nxsem_wait(&nwq->not_empty);
		if (ret == -EINTR) {
			continue;
		}
		if (ret < 0 || !nwq->running) {
			break;
		}

		while (nxmutex_lock(&nwq->lock) == -EINTR)
			;
		work = nwq->ring[nwq->tail];
		nwq->tail = (nwq->tail + 1) % OVE_WQ_QUEUE_DEPTH;
		nxmutex_unlock(&nwq->lock);
		nxsem_post(&nwq->not_full);

		if (work == NULL) {
			break; /* poison pill */
		}

		if (work->delay_ms > 0) {
			/* Sleep via tick wait on never-posted semaphore;
			 * interruptible by destroy posting delay_sem */
			nxsem_tickwait(&nwq->delay_sem, MSEC2TICK(work->delay_ms));
			work->delay_ms = 0;
			if (!nwq->running) {
				break;
			}
		}

		work->pending = 0;

		__atomic_store_n(&work->in_progress, 1, __ATOMIC_RELEASE);
		if (!work->cancelled) {
			work->handler(work);
		}
		__atomic_store_n(&work->in_progress, 0, __ATOMIC_RELEASE);
		/* Unconditional post — cancel/free's wait loop drains stale
		 * tokens before re-checking in_progress. */
		if (work->completion_sem_inited) {
			nxsem_post(&work->completion_sem);
		}
	}

	return 0;
}

/* Wait until the worker has fully released this work item.  See
 * backends/posix/posix_workqueue.c — same convergence rules: an
 * extra post from a prior iteration is harmless because the loop
 * re-checks in_progress before exiting. */
static void wait_for_completion(struct ove_work *w)
{
	if (!w->completion_sem_inited) {
		return;
	}
	while (__atomic_load_n(&w->in_progress, __ATOMIC_ACQUIRE)) {
		nxsem_wait_uninterruptible(&w->completion_sem);
	}
}

static int wq_start(struct ove_workqueue *nwq, const char *name, ove_prio_t priority,
		    size_t stack_size, void *stack)
{
	char addr_str[20];
	int pid;

	nxmutex_init(&nwq->lock);
	nxsem_init(&nwq->not_full, 0, OVE_WQ_QUEUE_DEPTH);
	nxsem_init(&nwq->not_empty, 0, 0);
	nxsem_init(&nwq->delay_sem, 0, 0);
	nwq->running = 1;
	nwq->head = 0;
	nwq->tail = 0;

	if (stack_size == 0) {
		stack_size = 2048;
	}

	(void)snprintf(addr_str, sizeof(addr_str), "0x%lx", (unsigned long)(uintptr_t)nwq);
	{
		char *argv_args[] = {addr_str, NULL};
		pid = task_create_with_stack(name ? name : "ove_wq",
					     ove_nuttx_map_priority(priority), stack,
					     (int)stack_size, wq_task_fn, argv_args);
	}
	if (pid < 0) {
		nxmutex_destroy(&nwq->lock);
		nxsem_destroy(&nwq->not_full);
		nxsem_destroy(&nwq->not_empty);
		nxsem_destroy(&nwq->delay_sem);
		return OVE_ERR_NO_MEMORY;
	}

	nwq->worker_pid = pid;
	return OVE_OK;
}

static void wq_stop(struct ove_workqueue *nwq)
{
	nwq->running = 0;
	/* Wake thread from delay sleep */
	nxsem_post(&nwq->delay_sem);
	/* Wake thread from receive wait (poison pill) */
	nxsem_post(&nwq->not_empty);
	task_delete(nwq->worker_pid);
	nxmutex_destroy(&nwq->lock);
	nxsem_destroy(&nwq->not_full);
	nxsem_destroy(&nwq->not_empty);
	nxsem_destroy(&nwq->delay_sem);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_workqueue_init(ove_workqueue_t *wq, ove_workqueue_storage_t *storage, const char *name,
		       ove_prio_t priority, size_t stack_size, void *stack)
{
	if (wq == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (stack != NULL && ((uintptr_t)stack & 7u) != 0u) {
		return OVE_ERR_INVALID_PARAM;
	}

	memset(storage, 0, sizeof(*storage));
	int ret = wq_start(storage, name, priority, stack_size, stack);
	if (ret != OVE_OK) {
		return ret;
	}

	*wq = storage;
	return OVE_OK;
}

void ove_workqueue_deinit(ove_workqueue_t wq)
{
	if (wq != NULL) {
		wq_stop(wq);
	}
}

int ove_work_init_static(ove_work_t *work, ove_work_storage_t *storage, ove_work_fn handler)
{
	if (work == NULL || storage == NULL || handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->handler = handler;
	storage->cancelled = 0;
	storage->delay_ms = 0;
	storage->pending = 0;
	storage->in_progress = 0;
	if (nxsem_init(&storage->completion_sem, 0, 0) == 0) {
		storage->completion_sem_inited = 1;
	} else {
		storage->completion_sem_inited = 0;
	}

	*work = storage;
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_WORKQUEUE
int ove_workqueue_create(ove_workqueue_t *wq, const char *name, ove_prio_t priority,
			 size_t stack_size)
{
	struct ove_workqueue *nwq;

	if (wq == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	nwq = OVE_BACKEND_MALLOC(sizeof(*nwq));
	if (nwq == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(nwq, 0, sizeof(*nwq));

	int ret = wq_start(nwq, name, priority, stack_size, NULL);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(nwq);
		return ret;
	}

	*wq = nwq;
	return OVE_OK;
}

void ove_workqueue_destroy(ove_workqueue_t wq)
{
	if (wq != NULL) {
		wq_stop(wq);
		OVE_BACKEND_FREE(wq);
	}
}
#endif /* OVE_HEAP_WORKQUEUE */

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_work_init(ove_work_t *work, ove_work_fn handler)
{
	struct ove_work *nw;

	if (work == NULL || handler == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	nw = OVE_BACKEND_MALLOC(sizeof(*nw));
	if (nw == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	memset(nw, 0, sizeof(*nw));
	nw->handler = handler;
	nw->cancelled = 0;
	nw->in_progress = 0;
	if (nxsem_init(&nw->completion_sem, 0, 0) == 0) {
		nw->completion_sem_inited = 1;
	} else {
		nw->completion_sem_inited = 0;
	}
	*work = nw;
	return OVE_OK;
}

void ove_work_free(ove_work_t work)
{
	if (work == NULL) {
		return;
	}
	/* Wait for any in-flight handler to finish before reclaiming the
	 * struct — closes the UAF window. */
	wait_for_completion(work);
	if (work->completion_sem_inited) {
		nxsem_destroy(&work->completion_sem);
		work->completion_sem_inited = 0;
	}
	OVE_BACKEND_FREE(work);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_work_submit(ove_workqueue_t wq, ove_work_t work)
{
	struct ove_workqueue *nwq = wq;
	struct ove_work *nw = work;

	nw->delay_ms = 0;
	nw->pending = 1;
	nw->cancelled = 0; /* clear a prior cancel so a resubmit runs again */

	int ret = nxsem_wait_uninterruptible(&nwq->not_full);
	if (ret < 0) {
		return OVE_ERR_TIMEOUT;
	}

	while (nxmutex_lock(&nwq->lock) == -EINTR)
		;
	nwq->ring[nwq->head] = nw;
	nwq->head = (nwq->head + 1) % OVE_WQ_QUEUE_DEPTH;
	nxmutex_unlock(&nwq->lock);

	nxsem_post(&nwq->not_empty);
	return OVE_OK;
}

int ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t work, uint32_t delay_ms)
{
	struct ove_workqueue *nwq = wq;
	struct ove_work *nw = work;

	nw->delay_ms = delay_ms;
	nw->pending = 1;
	nw->cancelled = 0; /* clear a prior cancel so a resubmit runs again */

	int ret = nxsem_wait_uninterruptible(&nwq->not_full);
	if (ret < 0) {
		return OVE_ERR_TIMEOUT;
	}

	while (nxmutex_lock(&nwq->lock) == -EINTR)
		;
	nwq->ring[nwq->head] = nw;
	nwq->head = (nwq->head + 1) % OVE_WQ_QUEUE_DEPTH;
	nxmutex_unlock(&nwq->lock);

	nxsem_post(&nwq->not_empty);
	return OVE_OK;
}

int ove_work_cancel(ove_work_t work)
{
	struct ove_work *nw = work;
	if (nw == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int was_pending = nw->pending;
	nw->cancelled = 1;
	nw->pending = 0;
	/* Wait for any in-flight handler to finish so the caller may
	 * safely free the struct after cancel returns. */
	wait_for_completion(nw);
	/* Contract: OVE_ERR_INVAL when the item was not pending. */
	return was_pending ? OVE_OK : OVE_ERR_INVAL;
}
