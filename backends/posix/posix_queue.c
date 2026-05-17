/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove/trace.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>
static void ns_to_abstime(uint64_t timeout_ns, struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += (time_t)(timeout_ns / 1000000000ULL);
	ts->tv_nsec += (long)(timeout_ns % 1000000000ULL);
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *storage, void *buffer, size_t item_size,
		   unsigned int max_items)
{
	if (item_size == 0 || max_items == 0)
		return OVE_ERR_INVALID_PARAM;
	struct ove_queue *sq = (struct ove_queue *)storage;
	memset(sq, 0, sizeof(*sq));
	sq->buffer = buffer;
	sq->item_size = item_size;
	sq->max_items = max_items;
	pthread_mutex_init(&sq->lock, NULL);
	pthread_cond_init(&sq->not_full, NULL);
	pthread_cond_init(&sq->not_empty, NULL);
	*q = sq;
	return OVE_OK;
}

void ove_queue_deinit(ove_queue_t q)
{
	struct ove_queue *sq = q;
	if (sq) {
		pthread_mutex_destroy(&sq->lock);
		pthread_cond_destroy(&sq->not_full);
		pthread_cond_destroy(&sq->not_empty);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_queue_create(ove_queue_t *q, size_t item_size, unsigned int max_items)
{
	if (item_size == 0 || max_items == 0)
		return OVE_ERR_INVALID_PARAM;
	struct ove_queue *sq = OVE_BACKEND_MALLOC(sizeof(*sq));
	if (!sq) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(sq, 0, sizeof(*sq));
	sq->buffer = OVE_BACKEND_MALLOC(item_size * max_items);
	if (!sq->buffer) {
		OVE_BACKEND_FREE(sq);
		return OVE_ERR_NO_MEMORY;
	}
	sq->item_size = item_size;
	sq->max_items = max_items;
	pthread_mutex_init(&sq->lock, NULL);
	pthread_cond_init(&sq->not_full, NULL);
	pthread_cond_init(&sq->not_empty, NULL);
	*q = sq;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_queue_destroy(ove_queue_t q)
{
	struct ove_queue *sq = q;
	if (sq) {
		pthread_mutex_destroy(&sq->lock);
		pthread_cond_destroy(&sq->not_full);
		pthread_cond_destroy(&sq->not_empty);
		OVE_BACKEND_FREE(sq->buffer);
		OVE_BACKEND_FREE(sq);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_queue_send(ove_queue_t q, const void *data, uint64_t timeout_ns)
{
	struct ove_queue *sq = q;
	pthread_mutex_lock(&sq->lock);

	if (timeout_ns == OVE_WAIT_FOREVER) {
		while (sq->count >= sq->max_items) {
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_WAIT_ENTER, sq);
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			pthread_cond_wait(&sq->not_full, &sq->lock);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_WAIT_EXIT, sq);
		}
	} else {
		struct timespec ts;
		ns_to_abstime(timeout_ns, &ts);
		while (sq->count >= sq->max_items) {
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_WAIT_ENTER, sq);
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			int ret = pthread_cond_timedwait(&sq->not_full, &sq->lock, &ts);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_WAIT_EXIT, sq);
			if (ret == ETIMEDOUT) {
				pthread_mutex_unlock(&sq->lock);
				return (timeout_ns == 0) ? OVE_ERR_QUEUE_FULL : OVE_ERR_TIMEOUT;
			}
		}
	}

	memcpy((char *)sq->buffer + sq->head * sq->item_size, data, sq->item_size);
	sq->head = (sq->head + 1) % sq->max_items;
	sq->count++;
	OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_POST, sq);
	pthread_cond_signal(&sq->not_empty);
	pthread_mutex_unlock(&sq->lock);
	return OVE_OK;
}

int ove_queue_receive(ove_queue_t q, void *buf, uint64_t timeout_ns)
{
	struct ove_queue *sq = q;
	pthread_mutex_lock(&sq->lock);

	if (timeout_ns == OVE_WAIT_FOREVER) {
		while (sq->count == 0) {
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_WAIT_ENTER, sq);
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			pthread_cond_wait(&sq->not_empty, &sq->lock);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_WAIT_EXIT, sq);
		}
	} else {
		struct timespec ts;
		ns_to_abstime(timeout_ns, &ts);
		while (sq->count == 0) {
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_WAIT_ENTER, sq);
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			int ret = pthread_cond_timedwait(&sq->not_empty, &sq->lock, &ts);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
			OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_WAIT_EXIT, sq);
			if (ret == ETIMEDOUT) {
				pthread_mutex_unlock(&sq->lock);
				return (timeout_ns == 0) ? OVE_ERR_QUEUE_EMPTY : OVE_ERR_TIMEOUT;
			}
		}
	}

	memcpy(buf, (char *)sq->buffer + sq->tail * sq->item_size, sq->item_size);
	sq->tail = (sq->tail + 1) % sq->max_items;
	sq->count--;
	pthread_cond_signal(&sq->not_full);
	pthread_mutex_unlock(&sq->lock);
	return OVE_OK;
}

int ove_queue_send_from_isr(ove_queue_t q, const void *data)
{
	return ove_queue_send(q, data, 0);
}

int ove_queue_receive_from_isr(ove_queue_t q, void *buf)
{
	struct ove_queue *sq = q;
	pthread_mutex_lock(&sq->lock);
	if (sq->count == 0) {
		pthread_mutex_unlock(&sq->lock);
		return OVE_ERR_QUEUE_EMPTY;
	}
	memcpy(buf, (char *)sq->buffer + sq->tail * sq->item_size, sq->item_size);
	sq->tail = (sq->tail + 1) % sq->max_items;
	sq->count--;
	pthread_cond_signal(&sq->not_full);
	pthread_mutex_unlock(&sq->lock);
	return OVE_OK;
}
