/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static void ms_to_abstime(uint32_t timeout_ms, struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += timeout_ms / 1000;
	ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *storage,
		       void *buffer, size_t item_size,
		       unsigned int max_items)
{
	if (!q || !storage || !buffer || item_size == 0 || max_items == 0)
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
int ove_queue_create(ove_queue_t *q, size_t item_size,
			 unsigned int max_items)
{
	if (!q || item_size == 0 || max_items == 0) return OVE_ERR_INVALID_PARAM;
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

int ove_queue_send(ove_queue_t q, const void *data,
		       uint32_t timeout_ms)
{
	struct ove_queue *sq = q;
	if (!sq || !data) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&sq->lock);

	if (timeout_ms == OVE_WAIT_FOREVER) {
		while (sq->count >= sq->max_items) {
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			pthread_cond_wait(&sq->not_full, &sq->lock);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
		}
	} else {
		struct timespec ts;
		ms_to_abstime(timeout_ms, &ts);
		while (sq->count >= sq->max_items) {
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			int ret = pthread_cond_timedwait(&sq->not_full,
							&sq->lock, &ts);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
			if (ret == ETIMEDOUT) {
				pthread_mutex_unlock(&sq->lock);
				return OVE_ERR_TIMEOUT;
			}
		}
	}

	memcpy((char *)sq->buffer + sq->head * sq->item_size, data,
	       sq->item_size);
	sq->head = (sq->head + 1) % sq->max_items;
	sq->count++;
	pthread_cond_signal(&sq->not_empty);
	pthread_mutex_unlock(&sq->lock);
	return OVE_OK;
}

int ove_queue_receive(ove_queue_t q, void *buf,
			  uint32_t timeout_ms)
{
	struct ove_queue *sq = q;
	if (!sq || !buf) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&sq->lock);

	if (timeout_ms == OVE_WAIT_FOREVER) {
		while (sq->count == 0) {
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			pthread_cond_wait(&sq->not_empty, &sq->lock);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
		}
	} else {
		struct timespec ts;
		ms_to_abstime(timeout_ms, &ts);
		while (sq->count == 0) {
			ove_backend_thread_set_state(OVE_THREAD_STATE_BLOCKED);
			int ret = pthread_cond_timedwait(&sq->not_empty,
							&sq->lock, &ts);
			ove_backend_thread_set_state(OVE_THREAD_STATE_RUNNING);
			if (ret == ETIMEDOUT) {
				pthread_mutex_unlock(&sq->lock);
				return OVE_ERR_TIMEOUT;
			}
		}
	}

	memcpy(buf, (char *)sq->buffer + sq->tail * sq->item_size,
	       sq->item_size);
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
	if (!sq || !buf) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&sq->lock);
	if (sq->count == 0) {
		pthread_mutex_unlock(&sq->lock);
		return OVE_ERR_TIMEOUT;
	}
	memcpy(buf, (char *)sq->buffer + sq->tail * sq->item_size,
	       sq->item_size);
	sq->tail = (sq->tail + 1) % sq->max_items;
	sq->count--;
	pthread_cond_signal(&sq->not_full);
	pthread_mutex_unlock(&sq->lock);
	return OVE_OK;
}
