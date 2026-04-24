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

int ove_stream_init(ove_stream_t *stream,
		        ove_stream_storage_t *storage,
		        void *buffer, size_t size, size_t trigger)
{
	if (!stream || !storage || !buffer || size == 0)
		return OVE_ERR_INVALID_PARAM;
	struct ove_stream *s = (struct ove_stream *)storage;
	memset(s, 0, sizeof(*s));
	s->buffer = buffer;
	s->size = size;
	s->trigger = trigger > 0 ? trigger : 1;
	pthread_mutex_init(&s->lock, NULL);
	pthread_cond_init(&s->data_avail, NULL);
	pthread_cond_init(&s->space_avail, NULL);
	*stream = s;
	return OVE_OK;
}

void ove_stream_deinit(ove_stream_t stream)
{
	struct ove_stream *s = stream;
	if (s) {
		pthread_mutex_destroy(&s->lock);
		pthread_cond_destroy(&s->data_avail);
		pthread_cond_destroy(&s->space_avail);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_stream_create(ove_stream_t *stream, size_t size,
			  size_t trigger)
{
	if (!stream || size == 0) return OVE_ERR_INVALID_PARAM;
	struct ove_stream *s = OVE_BACKEND_MALLOC(sizeof(*s));
	if (!s) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(s, 0, sizeof(*s));
	s->buffer = OVE_BACKEND_MALLOC(size);
	if (!s->buffer) {
		OVE_BACKEND_FREE(s);
		return OVE_ERR_NO_MEMORY;
	}
	s->size = size;
	s->trigger = trigger > 0 ? trigger : 1;
	pthread_mutex_init(&s->lock, NULL);
	pthread_cond_init(&s->data_avail, NULL);
	pthread_cond_init(&s->space_avail, NULL);
	*stream = s;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_stream_destroy(ove_stream_t stream)
{
	struct ove_stream *s = stream;
	if (s) {
		pthread_mutex_destroy(&s->lock);
		pthread_cond_destroy(&s->data_avail);
		pthread_cond_destroy(&s->space_avail);
		OVE_BACKEND_FREE(s->buffer);
		OVE_BACKEND_FREE(s);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_stream_send(ove_stream_t stream, const void *data,
		       size_t len, uint32_t timeout_ms,
		       size_t *bytes_sent)
{
	struct ove_stream *s = stream;
	if (!s || !data || len == 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	const uint8_t *src = data;
	size_t sent = 0;

	pthread_mutex_lock(&s->lock);

	struct timespec ts;
	if (timeout_ms != OVE_WAIT_FOREVER && timeout_ms > 0) {
		ms_to_abstime(timeout_ms, &ts);
	}

	while (sent < len) {
		while (s->count >= s->size) {
			if (timeout_ms == 0) {
				goto done;
			}
			int ret;
			if (timeout_ms == OVE_WAIT_FOREVER) {
				ret = pthread_cond_wait(&s->space_avail,
						       &s->lock);
			} else {
				ret = pthread_cond_timedwait(&s->space_avail,
							    &s->lock, &ts);
			}
			if (ret == ETIMEDOUT) {
				goto done;
			}
		}
		s->buffer[s->head] = src[sent++];
		s->head = (s->head + 1) % s->size;
		s->count++;
		if (s->count >= s->trigger) {
			pthread_cond_signal(&s->data_avail);
		}
	}
done:
	pthread_mutex_unlock(&s->lock);
	if (bytes_sent) {
		*bytes_sent = sent;
	}
	return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream, void *buf,
			   size_t len, uint32_t timeout_ms,
			   size_t *bytes_received)
{
	struct ove_stream *s = stream;
	if (!s || !buf || len == 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	uint8_t *dst = buf;
	size_t received = 0;

	pthread_mutex_lock(&s->lock);

	struct timespec ts;
	if (timeout_ms != OVE_WAIT_FOREVER && timeout_ms > 0) {
		ms_to_abstime(timeout_ms, &ts);
	}

	/* Wait for at least trigger bytes */
	while (s->count < s->trigger) {
		if (timeout_ms == 0) {
			goto done;
		}
		int ret;
		if (timeout_ms == OVE_WAIT_FOREVER) {
			ret = pthread_cond_wait(&s->data_avail, &s->lock);
		} else {
			ret = pthread_cond_timedwait(&s->data_avail, &s->lock,
						     &ts);
		}
		if (ret == ETIMEDOUT) {
			goto done;
		}
	}

	while (received < len && s->count > 0) {
		dst[received++] = s->buffer[s->tail];
		s->tail = (s->tail + 1) % s->size;
		s->count--;
	}
	pthread_cond_signal(&s->space_avail);
done:
	pthread_mutex_unlock(&s->lock);
	if (bytes_received) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream,
				 const void *data, size_t len,
				 size_t *bytes_sent)
{
	return ove_stream_send(stream, data, len, 0, bytes_sent);
}

int ove_stream_receive_from_isr(ove_stream_t stream,
				    void *buf, size_t len,
				    size_t *bytes_received)
{
	return ove_stream_receive(stream, buf, len, 0, bytes_received);
}

int ove_stream_reset(ove_stream_t stream)
{
	struct ove_stream *s = stream;
	if (!s) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&s->lock);
	s->head = 0;
	s->tail = 0;
	s->count = 0;
	pthread_mutex_unlock(&s->lock);
	return OVE_OK;
}

size_t ove_stream_bytes_available(ove_stream_t stream)
{
	struct ove_stream *s = stream;
	if (!s) {
		return 0;
	}
	pthread_mutex_lock(&s->lock);
	size_t avail = s->count;
	pthread_mutex_unlock(&s->lock);
	return avail;
}
