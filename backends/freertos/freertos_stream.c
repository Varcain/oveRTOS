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
#include "semphr.h"
#include "task.h"
#include <stdbool.h>
#include <string.h>

/*
 * Backed by a plain ring buffer rather than a FreeRTOS StreamBuffer: the
 * stream API is trigger-aware (a receive blocks until at least `trigger`
 * bytes are present, see ove/stream.h) but a StreamBuffer's trigger only
 * gates when the *sender* wakes a blocked reader — the reader still returns
 * whatever sub-trigger data is present. The ring is guarded by a critical
 * section (mutual exclusion between thread and ISR context) and two counting
 * semaphores provide the blocking wait/wake. Sem counts are only hints:
 * every waiter re-checks the ring under the lock after waking (a blocked
 * waiter always left its sem at 0, so the next give wakes it).
 */

/* ── ring helpers (caller holds the critical section) ─────────────────── */

static size_t ring_write(struct ove_stream *s, const unsigned char *src, size_t len)
{
	size_t avail = s->size - s->count;
	size_t chunk = (len < avail) ? len : avail;
	size_t contig = s->size - s->head;

	if (contig < chunk) {
		memcpy(s->buffer + s->head, src, contig);
		memcpy(s->buffer, src + contig, chunk - contig);
	} else {
		memcpy(s->buffer + s->head, src, chunk);
	}
	s->head = (s->head + chunk) % s->size;
	s->count += chunk;
	return chunk;
}

static size_t ring_read(struct ove_stream *s, unsigned char *dst, size_t len)
{
	size_t avail = s->count;
	size_t chunk = (len < avail) ? len : avail;
	size_t contig = s->size - s->tail;

	if (contig < chunk) {
		memcpy(dst, s->buffer + s->tail, contig);
		memcpy(dst + contig, s->buffer, chunk - contig);
	} else {
		memcpy(dst, s->buffer + s->tail, chunk);
	}
	s->tail = (s->tail + chunk) % s->size;
	s->count -= chunk;
	return chunk;
}

/* ── _init / _deinit ────────────────────────────────────────────────── */

int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger)
{
	if (stream == NULL || storage == NULL || buffer == NULL || size == 0)
		return OVE_ERR_INVALID_PARAM;

	storage->buffer = (unsigned char *)buffer;
	storage->size = size;
	storage->trigger = (trigger > 0) ? trigger : 1; /* 0 is treated as 1 */
	storage->head = 0;
	storage->tail = 0;
	storage->count = 0;
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	storage->data_sem = xSemaphoreCreateCountingStatic(size, 0, &storage->data_sem_buf);
	storage->space_sem = xSemaphoreCreateCountingStatic(size, 0, &storage->space_sem_buf);

	*stream = storage;
	return OVE_OK;
}

void ove_stream_deinit(ove_stream_t stream)
{
	(void)stream;
}

/* ── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_STREAM
int ove_stream_create(ove_stream_t *stream, size_t size, size_t trigger)
{
	struct ove_stream *w;

	if (stream == NULL || size == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (w == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	w->buffer = OVE_BACKEND_MALLOC(size);
	if (w->buffer == NULL) {
		OVE_BACKEND_FREE(w);
		return OVE_ERR_NO_MEMORY;
	}
	w->data_sem = xSemaphoreCreateCounting(size, 0);
	w->space_sem = xSemaphoreCreateCounting(size, 0);
	if (w->data_sem == NULL || w->space_sem == NULL) {
		if (w->data_sem != NULL)
			vSemaphoreDelete(w->data_sem);
		if (w->space_sem != NULL)
			vSemaphoreDelete(w->space_sem);
		OVE_BACKEND_FREE(w->buffer);
		OVE_BACKEND_FREE(w);
		return OVE_ERR_NO_MEMORY;
	}

	w->size = size;
	w->trigger = (trigger > 0) ? trigger : 1;
	w->head = 0;
	w->tail = 0;
	w->count = 0;
	w->notify_cb = NULL;
	w->notify_ud = NULL;

	*stream = w;
	return OVE_OK;
}

void ove_stream_destroy(ove_stream_t stream)
{
	if (stream != NULL) {
		vSemaphoreDelete(stream->data_sem);
		vSemaphoreDelete(stream->space_sem);
		OVE_BACKEND_FREE(stream->buffer);
		OVE_BACKEND_FREE(stream);
	}
}
#endif /* OVE_HEAP_STREAM */

/* ── Operations ─────────────────────────────────────────────────────── */

int ove_stream_send(ove_stream_t stream, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_sent)
{
	if (stream == NULL || data == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	const unsigned char *src = (const unsigned char *)data;
	size_t written = 0;
	TickType_t ticks = ove_ns_to_ticks(timeout_ns);
	TimeOut_t deadline;
	vTaskSetTimeOutState(&deadline);

	while (written < len) {
		bool wrote = false;
		bool have_trigger = false;

		taskENTER_CRITICAL();
		if (stream->count < stream->size) {
			written += ring_write(stream, src + written, len - written);
			have_trigger = stream->count >= stream->trigger;
			wrote = true;
		}
		taskEXIT_CRITICAL();

		if (wrote) {
			if (have_trigger) {
				(void)xSemaphoreGive(stream->data_sem);
			}
			continue;
		}
		if (xTaskCheckForTimeOut(&deadline, &ticks) != pdFALSE) {
			break;
		}
		(void)xSemaphoreTake(stream->space_sem, ticks);
	}

	if (written > 0 && stream->notify_cb != NULL) {
		stream->notify_cb(stream->notify_ud);
	}
	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}
	return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream, void *buf, size_t len, uint64_t timeout_ns,
		       size_t *bytes_received)
{
	if (stream == NULL || buf == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	size_t received = 0;
	TickType_t ticks = ove_ns_to_ticks(timeout_ns);
	TimeOut_t deadline;
	vTaskSetTimeOutState(&deadline);

	/* Block until at least `trigger` bytes are present, then drain up to
	 * `len` (whatever is available) without blocking again. */
	for (;;) {
		bool got = false;

		taskENTER_CRITICAL();
		if (stream->count >= stream->trigger) {
			received = ring_read(stream, (unsigned char *)buf, len);
			got = true;
		}
		taskEXIT_CRITICAL();

		if (got) {
			if (received > 0) {
				(void)xSemaphoreGive(stream->space_sem);
			}
			break;
		}
		if (xTaskCheckForTimeOut(&deadline, &ticks) != pdFALSE) {
			break;
		}
		(void)xSemaphoreTake(stream->data_sem, ticks);
	}

	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream, const void *data, size_t len, size_t *bytes_sent)
{
	if (stream == NULL || data == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	UBaseType_t state = taskENTER_CRITICAL_FROM_ISR();
	size_t written = ring_write(stream, (const unsigned char *)data, len);
	bool have_trigger = stream->count >= stream->trigger;
	taskEXIT_CRITICAL_FROM_ISR(state);

	BaseType_t yield = pdFALSE;
	if (written > 0 && have_trigger) {
		(void)xSemaphoreGiveFromISR(stream->data_sem, &yield);
	}
	if (written > 0 && stream->notify_cb != NULL) {
		stream->notify_cb(stream->notify_ud);
	}
	portYIELD_FROM_ISR(yield);
	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}
	return OVE_OK;
}

int ove_stream_receive_from_isr(ove_stream_t stream, void *buf, size_t len, size_t *bytes_received)
{
	if (stream == NULL || buf == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	UBaseType_t state = taskENTER_CRITICAL_FROM_ISR();
	size_t received = ring_read(stream, (unsigned char *)buf, len);
	taskEXIT_CRITICAL_FROM_ISR(state);

	BaseType_t yield = pdFALSE;
	if (received > 0) {
		(void)xSemaphoreGiveFromISR(stream->space_sem, &yield);
	}
	portYIELD_FROM_ISR(yield);
	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_reset(ove_stream_t stream)
{
	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	taskENTER_CRITICAL();
	stream->head = 0;
	stream->tail = 0;
	stream->count = 0;
	taskEXIT_CRITICAL();

	(void)xSemaphoreGive(stream->space_sem); /* wake any blocked sender */
	return OVE_OK;
}

size_t ove_stream_bytes_available(ove_stream_t stream)
{
	if (stream == NULL) {
		return 0;
	}

	taskENTER_CRITICAL();
	size_t count = stream->count;
	taskEXIT_CRITICAL();
	return count;
}

int ove_stream_set_notify(ove_stream_t stream, ove_notify_cb cb, void *user_data)
{
	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* Hooks are touched single-threaded at app init (typically before
	 * scheduler start) and then read from arbitrary contexts; the single
	 * fn-pointer store is naturally atomic on every supported arch. */
	stream->notify_cb = cb;
	stream->notify_ud = user_data;
	return OVE_OK;
}
