/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/stream.h"
#include "ove/storage.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>
static void ns_to_abstime(uint64_t ns, struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
	/* Fast path: ns < 4.29 s -> 32-bit divide (single-cycle UDIV
	 * on Cortex-M7).  Slow path stays a 64-bit divide. */
	if (ns <= (uint64_t)UINT32_MAX) {
		uint32_t n = (uint32_t)ns;
		ts->tv_sec += (time_t)(n / 1000000000u);
		ts->tv_nsec += (long)(n % 1000000000u);
	} else {
		ts->tv_sec += (time_t)(ns / 1000000000ULL);
		ts->tv_nsec += (long)(ns % 1000000000ULL);
	}
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger)
{
	if (stream == NULL || storage == NULL || buffer == NULL || size == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->buffer = (unsigned char *)buffer;
	storage->size = size;
	storage->trigger = (trigger > 0) ? trigger : 1; /* 0 is treated as 1 */
	storage->head = 0;
	storage->tail = 0;
	storage->count = 0;
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	pthread_mutex_init(&storage->lock, NULL);
	pthread_cond_init(&storage->not_empty, NULL);
	pthread_cond_init(&storage->not_full, NULL);

	*stream = storage;
	return OVE_OK;
}

void ove_stream_deinit(ove_stream_t stream)
{
	if (stream != NULL) {
		struct ove_stream *ns = stream;
		pthread_cond_destroy(&ns->not_full);
		pthread_cond_destroy(&ns->not_empty);
		pthread_mutex_destroy(&ns->lock);
	}
}

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_stream_send(ove_stream_t stream, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_sent)
{
	struct ove_stream *ns = stream;
	const unsigned char *src;
	size_t written = 0;

	if (ns == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	src = (const unsigned char *)data;

	pthread_mutex_lock(&ns->lock);

	while (written < len) {
		while (ns->count >= ns->size) {
			if (timeout_ns == OVE_WAIT_FOREVER) {
				pthread_cond_wait(&ns->not_full, &ns->lock);
			} else {
				struct timespec ts;
				ns_to_abstime(timeout_ns, &ts);
				int ret = pthread_cond_timedwait(&ns->not_full, &ns->lock, &ts);
				if (ret != 0) {
					goto out;
				}
			}
		}

		size_t avail = ns->size - ns->count;
		size_t chunk = len - written;
		if (chunk > avail)
			chunk = avail;

		size_t contig = ns->size - ns->head;
		if (contig < chunk) {
			memcpy(ns->buffer + ns->head, src + written, contig);
			memcpy(ns->buffer, src + written + contig, chunk - contig);
		} else {
			memcpy(ns->buffer + ns->head, src + written, chunk);
		}

		ns->head = (ns->head + chunk) % ns->size;
		ns->count += chunk;
		written += chunk;
	}

out:;
	ove_notify_cb notify_cb = (written > 0) ? ns->notify_cb : NULL;
	void *notify_ud = ns->notify_ud;
	if (written > 0) {
		pthread_cond_signal(&ns->not_empty);
	}

	pthread_mutex_unlock(&ns->lock);

	if (notify_cb) {
		notify_cb(notify_ud);
	}

	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}

	return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream, void *buf, size_t len, uint64_t timeout_ns,
		       size_t *bytes_received)
{
	struct ove_stream *ns = stream;
	unsigned char *dst;
	size_t read_bytes = 0;

	if (ns == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	dst = (unsigned char *)buf;

	pthread_mutex_lock(&ns->lock);

	while (read_bytes < len) {
		/* Trigger-aware: the first read blocks until at least `trigger`
		 * bytes are present; once draining has started (read_bytes > 0)
		 * any single byte suffices, returning what is available. */
		size_t min_avail = (read_bytes == 0) ? ns->trigger : 1;
		while (ns->count < min_avail) {
			if (read_bytes > 0) {
				goto out;
			}

			if (timeout_ns == OVE_WAIT_FOREVER) {
				pthread_cond_wait(&ns->not_empty, &ns->lock);
			} else {
				struct timespec ts;
				ns_to_abstime(timeout_ns, &ts);
				int ret = pthread_cond_timedwait(&ns->not_empty, &ns->lock, &ts);
				if (ret != 0) {
					goto out;
				}
			}
		}

		size_t avail = ns->count;
		size_t chunk = len - read_bytes;
		if (chunk > avail)
			chunk = avail;

		size_t contig = ns->size - ns->tail;
		if (contig < chunk) {
			memcpy(dst + read_bytes, ns->buffer + ns->tail, contig);
			memcpy(dst + read_bytes + contig, ns->buffer, chunk - contig);
		} else {
			memcpy(dst + read_bytes, ns->buffer + ns->tail, chunk);
		}

		ns->tail = (ns->tail + chunk) % ns->size;
		ns->count -= chunk;
		read_bytes += chunk;
	}

out:
	if (read_bytes > 0) {
		pthread_cond_signal(&ns->not_full);
	}

	pthread_mutex_unlock(&ns->lock);

	if (bytes_received != NULL) {
		*bytes_received = read_bytes;
	}

	return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream, const void *data, size_t len, size_t *bytes_sent)
{
	struct ove_stream *ns = stream;
	const unsigned char *src = (const unsigned char *)data;
	size_t written = 0;

	pthread_mutex_lock(&ns->lock);

	while (written < len && ns->count < ns->size) {
		size_t avail = ns->size - ns->count;
		size_t chunk = len - written;
		if (chunk > avail)
			chunk = avail;

		size_t contig = ns->size - ns->head;
		if (contig < chunk) {
			memcpy(ns->buffer + ns->head, src + written, contig);
			memcpy(ns->buffer, src + written + contig, chunk - contig);
		} else {
			memcpy(ns->buffer + ns->head, src + written, chunk);
		}

		ns->head = (ns->head + chunk) % ns->size;
		ns->count += chunk;
		written += chunk;
	}

	ove_notify_cb notify_cb = (written > 0) ? ns->notify_cb : NULL;
	void *notify_ud = ns->notify_ud;
	if (written > 0) {
		pthread_cond_signal(&ns->not_empty);
	}

	pthread_mutex_unlock(&ns->lock);

	if (notify_cb) {
		notify_cb(notify_ud);
	}

	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}

	return OVE_OK;
}

int ove_stream_receive_from_isr(ove_stream_t stream, void *buf, size_t len, size_t *bytes_received)
{
	struct ove_stream *ns = stream;
	unsigned char *dst = (unsigned char *)buf;
	size_t read_bytes = 0;

	pthread_mutex_lock(&ns->lock);

	while (read_bytes < len && ns->count > 0) {
		size_t avail = ns->count;
		size_t chunk = len - read_bytes;
		if (chunk > avail)
			chunk = avail;

		size_t contig = ns->size - ns->tail;
		if (contig < chunk) {
			memcpy(dst + read_bytes, ns->buffer + ns->tail, contig);
			memcpy(dst + read_bytes + contig, ns->buffer, chunk - contig);
		} else {
			memcpy(dst + read_bytes, ns->buffer + ns->tail, chunk);
		}

		ns->tail = (ns->tail + chunk) % ns->size;
		ns->count -= chunk;
		read_bytes += chunk;
	}

	if (read_bytes > 0) {
		pthread_cond_signal(&ns->not_full);
	}

	pthread_mutex_unlock(&ns->lock);

	if (bytes_received != NULL) {
		*bytes_received = read_bytes;
	}

	return OVE_OK;
}

int ove_stream_reset(ove_stream_t stream)
{
	struct ove_stream *ns = stream;

	pthread_mutex_lock(&ns->lock);
	ns->head = 0;
	ns->tail = 0;
	ns->count = 0;
	pthread_cond_signal(&ns->not_full);
	pthread_mutex_unlock(&ns->lock);

	return OVE_OK;
}

size_t ove_stream_bytes_available(ove_stream_t stream)
{
	struct ove_stream *ns = stream;
	size_t count;

	pthread_mutex_lock(&ns->lock);
	count = ns->count;
	pthread_mutex_unlock(&ns->lock);

	return count;
}

int ove_stream_set_notify(ove_stream_t stream, ove_notify_cb cb, void *user_data)
{
	struct ove_stream *ns = stream;
	if (ns == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&ns->lock);
	ns->notify_cb = cb;
	ns->notify_ud = user_data;
	pthread_mutex_unlock(&ns->lock);
	return OVE_OK;
}
