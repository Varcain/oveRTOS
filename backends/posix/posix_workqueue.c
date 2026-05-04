/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include "posix_sleep.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
static void *wq_thread_func(void *arg)
{
	struct ove_workqueue *wq = arg;

	while (1) {
		pthread_mutex_lock(&wq->lock);
		while (wq->count == 0 && wq->running) {
			pthread_cond_wait(&wq->cond, &wq->lock);
		}
		if (!wq->running && wq->count == 0) {
			pthread_mutex_unlock(&wq->lock);
			break;
		}
		struct ove_work *w = wq->queue[0];
		// queue is an array of struct ove_work *; the slot size IS the
		// pointer, so sizeof(queue[0]) is correct here.
		const size_t slot = sizeof(wq->queue[0]); // NOLINT(bugprone-sizeof-expression)
		memmove(&wq->queue[0], &wq->queue[1], (size_t)(wq->count - 1) * slot);
		wq->count--;
		/* Mark the work as being processed under the same lock that
		 * cancel/free synchronize on, so they observe in_progress=1
		 * even if they race in immediately after the dequeue. */
		w->in_progress = 1;
		pthread_mutex_unlock(&wq->lock);

		if (w->delay_ms > 0)
			posix_sleep_ms(w->delay_ms);

		/* Re-check pending — cancel may have flipped it during the
		 * delay sleep.  Snapshot under lock so the read is consistent
		 * with cancel's clearing store. */
		pthread_mutex_lock(&wq->lock);
		int still_pending = __atomic_load_n(&w->pending, __ATOMIC_ACQUIRE);
		__atomic_store_n(&w->pending, 0, __ATOMIC_RELEASE);
		pthread_mutex_unlock(&wq->lock);

		if (still_pending && w->handler) {
			w->handler(w);
		}

		/* Drop in_progress under the lock and broadcast — wakes any
		 * cancel/free waiters camped on wq->cond.  Broadcast (vs
		 * signal) because more than one thread may be waiting on
		 * different work items. */
		pthread_mutex_lock(&wq->lock);
		w->in_progress = 0;
		pthread_cond_broadcast(&wq->cond);
		pthread_mutex_unlock(&wq->lock);
	}
	return NULL;
}

int ove_workqueue_init(ove_workqueue_t *wqh, ove_workqueue_storage_t *storage, const char *name,
		       ove_prio_t priority, size_t stack_size, void *stack)
{
	(void)name;
	(void)priority;
	(void)stack_size;
	(void)stack;

	if (!wqh || !storage)
		return OVE_ERR_INVALID_PARAM;
	struct ove_workqueue *wq = (struct ove_workqueue *)storage;
	memset(wq, 0, sizeof(*wq));
	pthread_mutex_init(&wq->lock, NULL);
	pthread_cond_init(&wq->cond, NULL);
	wq->running = 1;

	if (pthread_create(&wq->thread, NULL, wq_thread_func, wq) != 0) {
		pthread_mutex_destroy(&wq->lock);
		pthread_cond_destroy(&wq->cond);
		return OVE_ERR_NO_MEMORY;
	}

	*wqh = wq;
	return OVE_OK;
}

void ove_workqueue_deinit(ove_workqueue_t wqh)
{
	struct ove_workqueue *wq = wqh;
	if (!wq) {
		return;
	}
	pthread_mutex_lock(&wq->lock);
	wq->running = 0;
	pthread_cond_signal(&wq->cond);
	pthread_mutex_unlock(&wq->lock);
	pthread_join(wq->thread, NULL);
	pthread_mutex_destroy(&wq->lock);
	pthread_cond_destroy(&wq->cond);
}

int ove_work_init_static(ove_work_t *work, ove_work_storage_t *storage, ove_work_fn handler)
{
	if (!work || !storage || !handler)
		return OVE_ERR_INVALID_PARAM;
	struct ove_work *w = (struct ove_work *)storage;
	memset(w, 0, sizeof(*w));
	w->handler = handler;
	*work = w;
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_workqueue_create(ove_workqueue_t *wqh, const char *name, ove_prio_t priority,
			 size_t stack_size)
{
	(void)name;
	(void)priority;
	(void)stack_size;

	if (!wqh)
		return OVE_ERR_INVALID_PARAM;
	struct ove_workqueue *wq = OVE_BACKEND_MALLOC(sizeof(*wq));
	if (!wq) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(wq, 0, sizeof(*wq));
	pthread_mutex_init(&wq->lock, NULL);
	pthread_cond_init(&wq->cond, NULL);
	wq->running = 1;

	if (pthread_create(&wq->thread, NULL, wq_thread_func, wq) != 0) {
		pthread_mutex_destroy(&wq->lock);
		pthread_cond_destroy(&wq->cond);
		OVE_BACKEND_FREE(wq);
		return OVE_ERR_NO_MEMORY;
	}

	*wqh = wq;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_workqueue_destroy(ove_workqueue_t wqh)
{
	struct ove_workqueue *wq = wqh;
	if (!wq) {
		return;
	}
	pthread_mutex_lock(&wq->lock);
	wq->running = 0;
	pthread_cond_signal(&wq->cond);
	pthread_mutex_unlock(&wq->lock);
	pthread_join(wq->thread, NULL);
	pthread_mutex_destroy(&wq->lock);
	pthread_cond_destroy(&wq->cond);
	OVE_BACKEND_FREE(wq);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_work_init(ove_work_t *work, ove_work_fn handler)
{
	if (!work || !handler)
		return OVE_ERR_INVALID_PARAM;
	struct ove_work *w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (!w) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(w, 0, sizeof(*w));
	w->handler = handler;
	*work = w;
	return OVE_OK;
}

void ove_work_free(ove_work_t work)
{
	struct ove_work *w = work;
	if (!w) {
		return;
	}
	/* If this work was ever submitted, the worker may still be running
	 * the handler — wait for in_progress to drop before freeing the
	 * struct, otherwise we hand the worker a use-after-free. */
	struct ove_workqueue *wq = w->wq;
	if (wq) {
		pthread_mutex_lock(&wq->lock);
		while (w->in_progress) {
			pthread_cond_wait(&wq->cond, &wq->lock);
		}
		pthread_mutex_unlock(&wq->lock);
	}
	OVE_BACKEND_FREE(w);
}

int ove_work_submit(ove_workqueue_t wqh, ove_work_t work)
{
	struct ove_workqueue *wq = wqh;
	struct ove_work *w = work;
	if (!wq || !w) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&wq->lock);
	if (wq->count >= OVE_WQ_MAX_PENDING) {
		pthread_mutex_unlock(&wq->lock);
		return OVE_ERR_NO_MEMORY;
	}
	w->delay_ms = 0;
	w->wq = wq; /* backpointer for cancel/free synchronization */
	__atomic_store_n(&w->pending, 1, __ATOMIC_RELEASE);
	wq->queue[wq->count++] = w;
	pthread_cond_signal(&wq->cond);
	pthread_mutex_unlock(&wq->lock);
	return OVE_OK;
}

int ove_work_submit_delayed(ove_workqueue_t wqh, ove_work_t work, uint32_t delay_ms)
{
	struct ove_workqueue *wq = wqh;
	struct ove_work *w = work;
	if (!wq || !w) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&wq->lock);
	if (wq->count >= OVE_WQ_MAX_PENDING) {
		pthread_mutex_unlock(&wq->lock);
		return OVE_ERR_NO_MEMORY;
	}
	w->delay_ms = delay_ms;
	w->wq = wq; /* backpointer for cancel/free synchronization */
	__atomic_store_n(&w->pending, 1, __ATOMIC_RELEASE);
	wq->queue[wq->count++] = w;
	pthread_cond_signal(&wq->cond);
	pthread_mutex_unlock(&wq->lock);
	return OVE_OK;
}

int ove_work_cancel(ove_work_t work)
{
	struct ove_work *w = work;
	if (!w) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct ove_workqueue *wq = w->wq;
	if (!wq) {
		/* Never submitted — nothing to cancel. */
		return OVE_ERR_NOT_SUPPORTED;
	}
	pthread_mutex_lock(&wq->lock);
	int was_pending = __atomic_load_n(&w->pending, __ATOMIC_ACQUIRE);
	__atomic_store_n(&w->pending, 0, __ATOMIC_RELEASE);
	/* Wait for any in-flight execution of THIS work item to finish.
	 * Without this, the caller could free the work while the worker
	 * is still inside w->handler (the UAF TSan caught). */
	while (w->in_progress) {
		pthread_cond_wait(&wq->cond, &wq->lock);
	}
	pthread_mutex_unlock(&wq->lock);
	return was_pending ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}
