/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
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

int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger)
{
	if (!stream || !storage || !buffer || size == 0)
		return OVE_ERR_INVALID_PARAM;
	struct ove_stream *s = (struct ove_stream *)storage;
	memset(s, 0, sizeof(*s));
	ove_ring_setup(&s->ring, buffer, size);
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

int ove_stream_send(ove_stream_t stream, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_sent)
{
	struct ove_stream *s = stream;
	if (!s || (!data && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_sent) {
			*bytes_sent = 0;
		}
		return OVE_OK;
	}
	const uint8_t *src = data;
	size_t sent = 0;

	pthread_mutex_lock(&s->lock);

	struct timespec ts;
	if (timeout_ns != OVE_WAIT_FOREVER && timeout_ns > 0) {
		ns_to_abstime(timeout_ns, &ts);
	}

	while (sent < len) {
		while (s->ring.count >= s->ring.size) {
			if (timeout_ns == 0) {
				goto done;
			}
			int ret;
			if (timeout_ns == OVE_WAIT_FOREVER) {
				ret = pthread_cond_wait(&s->space_avail, &s->lock);
			} else {
				ret = pthread_cond_timedwait(&s->space_avail, &s->lock, &ts);
			}
			if (ret == ETIMEDOUT) {
				goto done;
			}
		}
		sent += ove_ring_write(&s->ring, src + sent, len - sent);
		if (s->ring.count >= s->trigger) {
			pthread_cond_signal(&s->data_avail);
		}
	}
done:;
	/* Snapshot the notify hook under the lock so we can fire it after
	 * unlocking — callbacks (e.g. Rust AtomicWaker::wake) must not run
	 * with the stream mutex held in case the wake path re-enters. */
	ove_notify_cb notify_cb = (sent > 0) ? s->notify_cb : NULL;
	void *notify_ud = s->notify_ud;
	pthread_mutex_unlock(&s->lock);
	if (notify_cb) {
		notify_cb(notify_ud);
	}
	if (bytes_sent) {
		*bytes_sent = sent;
	}
	return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream, void *buf, size_t len, uint64_t timeout_ns,
		       size_t *bytes_received)
{
	struct ove_stream *s = stream;
	if (!s || (!buf && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_received) {
			*bytes_received = 0;
		}
		return OVE_OK;
	}
	uint8_t *dst = buf;
	size_t received = 0;

	pthread_mutex_lock(&s->lock);

	struct timespec ts;
	if (timeout_ns != OVE_WAIT_FOREVER && timeout_ns > 0) {
		ns_to_abstime(timeout_ns, &ts);
	}

	/* Wait for at least trigger bytes */
	while (s->ring.count < s->trigger) {
		if (timeout_ns == 0) {
			goto done;
		}
		int ret;
		if (timeout_ns == OVE_WAIT_FOREVER) {
			ret = pthread_cond_wait(&s->data_avail, &s->lock);
		} else {
			ret = pthread_cond_timedwait(&s->data_avail, &s->lock, &ts);
		}
		if (ret == ETIMEDOUT) {
			goto done;
		}
	}

	received = ove_ring_read(&s->ring, dst, len);
	pthread_cond_signal(&s->space_avail);
done:
	pthread_mutex_unlock(&s->lock);
	if (bytes_received) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream, const void *data, size_t len, size_t *bytes_sent)
{
	return ove_stream_send(stream, data, len, 0, bytes_sent);
}

/* Takes whatever is there, like the engines' interrupt path: the trigger
 * level only gates a waiting receive. */
int ove_stream_receive_from_isr(ove_stream_t stream, void *buf, size_t len, size_t *bytes_received)
{
	struct ove_stream *s = stream;
	if (!s || (!buf && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	size_t received = 0;
	if (len > 0) {
		pthread_mutex_lock(&s->lock);
		received = ove_ring_read(&s->ring, buf, len);
		if (received > 0) {
			pthread_cond_signal(&s->space_avail);
		}
		pthread_mutex_unlock(&s->lock);
	}
	if (bytes_received) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_reset(ove_stream_t stream)
{
	struct ove_stream *s = stream;
	if (!s) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&s->lock);
	ove_ring_clear(&s->ring);
	pthread_cond_signal(&s->space_avail); /* wake a blocked sender, as the engines do */
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
	size_t avail = s->ring.count;
	pthread_mutex_unlock(&s->lock);
	return avail;
}

size_t ove_stream_space_available(ove_stream_t stream)
{
	struct ove_stream *s = stream;
	if (!s) {
		return 0;
	}
	pthread_mutex_lock(&s->lock);
	size_t space = s->ring.size - s->ring.count;
	pthread_mutex_unlock(&s->lock);
	return space;
}

int ove_stream_set_notify(ove_stream_t stream, ove_notify_cb cb, void *user_data)
{
	struct ove_stream *s = stream;
	if (!s) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&s->lock);
	s->notify_cb = cb;
	s->notify_ud = user_data;
	pthread_mutex_unlock(&s->lock);
	return OVE_OK;
}
