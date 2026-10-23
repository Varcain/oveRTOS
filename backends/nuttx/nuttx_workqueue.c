/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/workqueue.h"
#include "ove/storage.h"
#include "ove_nuttx_priority.h"
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

enum {
	WORK_IDLE,
	WORK_QUEUED,
	WORK_RUNNING,
	WORK_CANCELLED,
};

static int wq_task_fn(int argc, char *argv[])
{
	struct ove_workqueue *nwq;
	struct ove_work *work;
	irqstate_t flags;

	(void)argc;
	nwq = (struct ove_workqueue *)strtoul(argv[1], NULL, 0);

	while (1) {
		int ret = nxsem_wait(&nwq->not_empty);
		if (ret == -EINTR) {
			continue;
		}
		if (ret < 0 || !nwq->running) {
			break;
		}

		flags = enter_critical_section();
		if (nwq->count == 0) {
			leave_critical_section(flags);
			continue;
		}
		work = nwq->ring[nwq->tail];
		nwq->tail = (nwq->tail + 1) % OVE_WQ_QUEUE_DEPTH;
		nwq->count--;
		nwq->active_work = work;
		leave_critical_section(flags);
		nxsem_post(&nwq->not_full);

		if (work == NULL) {
			break; /* poison pill */
		}

		flags = enter_critical_section();
		int delay = work->delay_ms > 0 && nwq->running &&
			    __atomic_load_n(&work->state, __ATOMIC_ACQUIRE) == WORK_QUEUED;
		nwq->active_delaying = delay;
		leave_critical_section(flags);
		if (delay) {
			/* Sleep via tick wait on never-posted semaphore;
			 * interruptible by destroy posting delay_sem */
			nxsem_tickwait(&nwq->delay_sem, MSEC2TICK(work->delay_ms));
		}
		flags = enter_critical_section();
		nwq->active_delaying = 0;
		/* A cancel may have posted after the timeout expired but before
		 * this critical section. Do not let that stale wake shorten the next
		 * delayed item handled by this single worker. */
		(void)nxsem_trywait(&nwq->delay_sem);
		leave_critical_section(flags);
		work->delay_ms = 0;

		int expected = WORK_QUEUED;
		if (nwq->running &&
		    __atomic_compare_exchange_n(&work->state, &expected, WORK_RUNNING, 0,
						__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			work->handler(work);
		}

		flags = enter_critical_section();
		nwq->active_work = NULL;
		work->wq = NULL;
		__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
		leave_critical_section(flags);
		if (work->completion_sem_inited) {
			nxsem_post(&work->completion_sem);
		}
	}

	nxsem_post(&nwq->stopped_sem);
	return 0;
}

/* Wait until the worker has removed every asynchronous reference. Stale
 * semaphore tokens are harmless because the state is rechecked. */
static void wait_for_completion(struct ove_work *w)
{
	if (!w->completion_sem_inited) {
		return;
	}
	while (__atomic_load_n(&w->state, __ATOMIC_ACQUIRE) != WORK_IDLE) {
		nxsem_wait_uninterruptible(&w->completion_sem);
	}
}

static int wq_start(struct ove_workqueue *nwq, const char *name, ove_prio_t priority,
		    size_t stack_size, void *stack)
{
	char addr_str[20];
	int pid;

	nxsem_init(&nwq->not_full, 0, OVE_WQ_QUEUE_DEPTH);
	nxsem_init(&nwq->not_empty, 0, 0);
#ifdef CONFIG_PRIORITY_INHERITANCE
	/* Counting semaphores an interrupt takes or gives must not track holders
	 * (nxsem_init already leaves them so; this keeps it if that changes),
	 * or an interrupt's try-wait would record the task it preempted. */
	nxsem_set_protocol(&nwq->not_full, SEM_PRIO_NONE);
	nxsem_set_protocol(&nwq->not_empty, SEM_PRIO_NONE);
#endif
	nxsem_init(&nwq->delay_sem, 0, 0);
	nxsem_init(&nwq->stopped_sem, 0, 0);
	nwq->running = 1;
	nwq->head = 0;
	nwq->tail = 0;
	nwq->count = 0;
	nwq->active_work = NULL;
	nwq->active_delaying = 0;

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
		nxsem_destroy(&nwq->not_full);
		nxsem_destroy(&nwq->not_empty);
		nxsem_destroy(&nwq->delay_sem);
		nxsem_destroy(&nwq->stopped_sem);
		return OVE_ERR_NO_MEMORY;
	}

	nwq->worker_pid = pid;
	return OVE_OK;
}

static void wq_stop(struct ove_workqueue *nwq)
{
	irqstate_t flags;

	nwq->running = 0;
	/* Wake thread from delay sleep */
	nxsem_post(&nwq->delay_sem);
	/* Wake thread from receive wait (poison pill) */
	nxsem_post(&nwq->not_empty);
	nxsem_wait_uninterruptible(&nwq->stopped_sem);
	task_delete(nwq->worker_pid);

	/* Publish completion for entries the stopped worker never dequeued. */
	flags = enter_critical_section();
	while (nwq->count > 0) {
		struct ove_work *work = nwq->ring[nwq->tail];
		nwq->tail = (nwq->tail + 1) % OVE_WQ_QUEUE_DEPTH;
		nwq->count--;
		work->wq = NULL;
		__atomic_store_n(&work->state, WORK_IDLE, __ATOMIC_RELEASE);
		if (work->completion_sem_inited)
			nxsem_post(&work->completion_sem);
	}
	leave_critical_section(flags);
	nxsem_destroy(&nwq->not_full);
	nxsem_destroy(&nwq->not_empty);
	nxsem_destroy(&nwq->delay_sem);
	nxsem_destroy(&nwq->stopped_sem);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_workqueue_init(ove_workqueue_t *wq, ove_workqueue_storage_t *storage, const char *name,
		       ove_prio_t priority, size_t stack_size, void *stack)
{
	if (wq == NULL || storage == NULL || (unsigned)priority > OVE_PRIO_MAX) {
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
	storage->delay_ms = 0;
	storage->state = WORK_IDLE;
	storage->wq = NULL;
	if (nxsem_init(&storage->completion_sem, 0, 0) != 0)
		return OVE_ERR_NO_MEMORY;
	storage->completion_sem_inited = 1;

	*work = storage;
	return OVE_OK;
}

void ove_work_deinit(ove_work_t work)
{
	if (work == NULL) {
		return;
	}
	(void)ove_work_cancel(work);
	if (work->completion_sem_inited) {
		nxsem_destroy(&work->completion_sem);
		work->completion_sem_inited = 0;
	}
	work->handler = NULL;
}

/* ─── Operations ─────────────────────────────────────────────────────── */

static int submit_with_delay(ove_workqueue_t wq, ove_work_t work, uint32_t delay_ms)
{
	struct ove_workqueue *nwq = wq;
	struct ove_work *nw = work;
	irqstate_t flags;
	if (nwq == NULL || nw == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (!nwq->running)
		return OVE_ERR_BUSY;

	int expected = WORK_IDLE;
	if (!__atomic_compare_exchange_n(&nw->state, &expected, WORK_QUEUED, 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		return OVE_ERR_BUSY; /* not ours: leave its delay alone */
	nw->delay_ms = delay_ms;
	nw->wq = nwq;

	int ret = nxsem_wait_uninterruptible(&nwq->not_full);
	if (ret < 0) {
		nw->wq = NULL;
		__atomic_store_n(&nw->state, WORK_IDLE, __ATOMIC_RELEASE);
		return OVE_ERR_TIMEOUT;
	}

	flags = enter_critical_section();
	if (!nwq->running) {
		leave_critical_section(flags);
		nxsem_post(&nwq->not_full);
		nw->wq = NULL;
		__atomic_store_n(&nw->state, WORK_IDLE, __ATOMIC_RELEASE);
		return OVE_ERR_BUSY;
	}
	nwq->ring[nwq->head] = nw;
	nwq->head = (nwq->head + 1) % OVE_WQ_QUEUE_DEPTH;
	nwq->count++;
	leave_critical_section(flags);

	nxsem_post(&nwq->not_empty);
	return OVE_OK;
}

int ove_work_submit(ove_workqueue_t wq, ove_work_t work)
{
	return submit_with_delay(wq, work, 0);
}

/* The thread path above without its one wait: a free slot is taken with a
 * try-wait, and the ring is guarded by a critical section, which an
 * interrupt can enter, not a mutex. */
int ove_work_submit_from_isr(ove_workqueue_t wq, ove_work_t work)
{
	struct ove_workqueue *nwq = wq;
	struct ove_work *nw = work;
	if (nwq == NULL || nw == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (!nwq->running)
		return OVE_ERR_BUSY;
	int expected = WORK_IDLE;
	if (!__atomic_compare_exchange_n(&nw->state, &expected, WORK_QUEUED, 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		return OVE_ERR_BUSY;
	nw->delay_ms = 0;
	nw->wq = nwq;

	if (nxsem_trywait(&nwq->not_full) < 0) {
		nw->wq = NULL;
		__atomic_store_n(&nw->state, WORK_IDLE, __ATOMIC_RELEASE);
		return OVE_ERR_QUEUE_FULL;
	}

	irqstate_t flags = enter_critical_section();
	if (!nwq->running) {
		leave_critical_section(flags);
		nxsem_post(&nwq->not_full);
		nw->wq = NULL;
		__atomic_store_n(&nw->state, WORK_IDLE, __ATOMIC_RELEASE);
		return OVE_ERR_BUSY;
	}
	nwq->ring[nwq->head] = nw;
	nwq->head = (nwq->head + 1) % OVE_WQ_QUEUE_DEPTH;
	nwq->count++;
	leave_critical_section(flags);

	nxsem_post(&nwq->not_empty);
	return OVE_OK;
}

int ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t work, uint32_t delay_ms)
{
	return submit_with_delay(wq, work, delay_ms);
}

int ove_work_cancel(ove_work_t work)
{
	struct ove_work *nw = work;
	if (nw == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	int state = __atomic_load_n(&nw->state, __ATOMIC_ACQUIRE);
	if (state == WORK_IDLE)
		return OVE_ERR_INVAL;

	int cancelled = 0;
	if (state == WORK_QUEUED) {
		int expected = WORK_QUEUED;
		cancelled = __atomic_compare_exchange_n(&nw->state, &expected, WORK_CANCELLED, 0,
							__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	}

	struct ove_workqueue *nwq = nw->wq;
	if (cancelled && nwq != NULL) {
		irqstate_t flags = enter_critical_section();
		int active = nwq->active_work == nw && nwq->active_delaying;
		leave_critical_section(flags);
		if (active)
			nxsem_post(&nwq->delay_sem);
	}
	wait_for_completion(nw);
	return cancelled ? OVE_OK : OVE_ERR_INVAL;
}

/* WORK_QUEUED covers both queued and delayed items: the worker sleeps a
 * delay out before it starts the handler. */
bool ove_work_is_pending(ove_work_t work)
{
	struct ove_work *nw = work;
	if (nw == NULL) {
		return false;
	}
	return __atomic_load_n(&nw->state, __ATOMIC_ACQUIRE) == WORK_QUEUED;
}
