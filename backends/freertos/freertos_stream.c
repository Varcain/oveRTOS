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
#include "FreeRTOS.h"
#include "ove_ns_to_ticks.h"
#include "stream_buffer.h"

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger)
{
	if (stream == NULL || storage == NULL || buffer == NULL || size == 0)
		return OVE_ERR_INVALID_PARAM;
	if (trigger == 0 || trigger > size)
		return OVE_ERR_INVALID_PARAM;

	storage->handle = xStreamBufferCreateStatic(size, trigger, (uint8_t *)buffer,
						    &storage->static_stream);
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	*stream = storage;
	return OVE_OK;
}

void ove_stream_deinit(ove_stream_t stream)
{
	if (stream != NULL) {
		vStreamBufferDelete(stream->handle);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_STREAM
int ove_stream_create(ove_stream_t *stream, size_t size, size_t trigger)
{
	struct ove_stream *w;

	if (stream == NULL || size == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (trigger == 0) {
		trigger = 1;
	}

	w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (w == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	w->handle = xStreamBufferCreate(size, trigger);
	if (w->handle == NULL) {
		OVE_BACKEND_FREE(w);
		return OVE_ERR_NO_MEMORY;
	}
	w->notify_cb = NULL;
	w->notify_ud = NULL;

	*stream = w;
	return OVE_OK;
}

void ove_stream_destroy(ove_stream_t stream)
{
	if (stream != NULL) {
		vStreamBufferDelete(stream->handle);
		OVE_BACKEND_FREE(stream);
	}
}
#endif /* OVE_HEAP_STREAM */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_stream_send(ove_stream_t stream, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_sent)
{
	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	size_t sent = xStreamBufferSend(stream->handle, data, len, ove_ns_to_ticks(timeout_ns));
	if (sent > 0 && stream->notify_cb != NULL) {
		stream->notify_cb(stream->notify_ud);
	}
	if (bytes_sent != NULL) {
		*bytes_sent = sent;
	}
	return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream, void *buf, size_t len, uint64_t timeout_ns,
		       size_t *bytes_received)
{
	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	size_t received =
		xStreamBufferReceive(stream->handle, buf, len, ove_ns_to_ticks(timeout_ns));
	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream, const void *data, size_t len, size_t *bytes_sent)
{
	BaseType_t yield = pdFALSE;
	size_t sent;

	sent = xStreamBufferSendFromISR(stream->handle, data, len, &yield);
	if (sent > 0 && stream->notify_cb != NULL) {
		stream->notify_cb(stream->notify_ud);
	}
	portYIELD_FROM_ISR(yield);
	if (bytes_sent != NULL) {
		*bytes_sent = sent;
	}
	return OVE_OK;
}

int ove_stream_receive_from_isr(ove_stream_t stream, void *buf, size_t len, size_t *bytes_received)
{
	BaseType_t yield = pdFALSE;
	size_t received;

	received = xStreamBufferReceiveFromISR(stream->handle, buf, len, &yield);
	portYIELD_FROM_ISR(yield);
	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_reset(ove_stream_t stream)
{
	if (xStreamBufferReset(stream->handle) == pdPASS) {
		return OVE_OK;
	}
	return OVE_ERR_TIMEOUT;
}

size_t ove_stream_bytes_available(ove_stream_t stream)
{
	return xStreamBufferBytesAvailable(stream->handle);
}

int ove_stream_set_notify(ove_stream_t stream, ove_notify_cb cb, void *user_data)
{
	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* Hooks are touched single-threaded at app init (typically before
	 * scheduler start) and then read from arbitrary contexts; the
	 * single-fn-pointer store is naturally atomic on every supported
	 * arch and the C-side never races with itself. */
	stream->notify_cb = cb;
	stream->notify_ud = user_data;
	return OVE_OK;
}
