/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/stream.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <string.h>
/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger)
{
	(void)trigger;

	if (stream == NULL || storage == NULL || buffer == NULL || size == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->buffer = (unsigned char *)buffer;
	storage->size = size;
	atomic_set(&storage->bytes_count, 0);
	k_pipe_init(&storage->pipe, storage->buffer, size);

	*stream = storage;
	return OVE_OK;
}

void ove_stream_deinit(ove_stream_t stream)
{
	if (stream != NULL) {
		k_pipe_reset(&stream->pipe);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_STREAM
int ove_stream_create(ove_stream_t *stream, size_t size, size_t trigger)
{
	struct ove_stream *zs;

	(void)trigger;

	if (stream == NULL || size == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	zs = OVE_BACKEND_MALLOC(sizeof(*zs) + size);
	if (zs == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	zs->buffer = (unsigned char *)zs + sizeof(*zs);

	zs->size = size;
	atomic_set(&zs->bytes_count, 0);
	k_pipe_init(&zs->pipe, zs->buffer, size);

	*stream = zs;
	return OVE_OK;
}

void ove_stream_destroy(ove_stream_t stream)
{
	if (stream != NULL) {
		OVE_BACKEND_FREE(stream);
	}
}
#endif /* OVE_HEAP_STREAM */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_stream_send(ove_stream_t stream, const void *data, size_t len, uint32_t timeout_ms,
		    size_t *bytes_sent)
{
	k_timeout_t timeout;
	int ret;

	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (timeout_ms == OVE_WAIT_FOREVER) {
		timeout = K_FOREVER;
	} else {
		timeout = K_MSEC(timeout_ms);
	}

	ret = k_pipe_write(&stream->pipe, (const uint8_t *)data, len, timeout);
	size_t written = (ret > 0) ? (size_t)ret : 0;
	if (written > 0) {
		atomic_add(&stream->bytes_count, written);
	}
	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}
	return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream, void *buf, size_t len, uint32_t timeout_ms,
		       size_t *bytes_received)
{
	k_timeout_t timeout;
	int ret;

	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (timeout_ms == OVE_WAIT_FOREVER) {
		timeout = K_FOREVER;
	} else {
		timeout = K_MSEC(timeout_ms);
	}

	ret = k_pipe_read(&stream->pipe, (uint8_t *)buf, len, timeout);
	size_t received = (ret > 0) ? (size_t)ret : 0;
	if (received > 0) {
		atomic_sub(&stream->bytes_count, received);
	}
	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream, const void *data, size_t len, size_t *bytes_sent)
{
	int ret;

	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	ret = k_pipe_write(&stream->pipe, (const uint8_t *)data, len, K_NO_WAIT);
	size_t written = (ret > 0) ? (size_t)ret : 0;
	if (written > 0) {
		atomic_add(&stream->bytes_count, written);
	}
	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}
	return OVE_OK;
}

int ove_stream_receive_from_isr(ove_stream_t stream, void *buf, size_t len, size_t *bytes_received)
{
	int ret;

	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	ret = k_pipe_read(&stream->pipe, (uint8_t *)buf, len, K_NO_WAIT);
	size_t received = (ret > 0) ? (size_t)ret : 0;
	if (received > 0) {
		atomic_sub(&stream->bytes_count, received);
	}
	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_reset(ove_stream_t stream)
{
	k_pipe_reset(&stream->pipe);
	atomic_set(&stream->bytes_count, 0);
	return OVE_OK;
}

size_t ove_stream_bytes_available(ove_stream_t stream)
{
	atomic_val_t val = atomic_get(&stream->bytes_count);
	return (val > 0) ? (size_t)val : 0;
}
