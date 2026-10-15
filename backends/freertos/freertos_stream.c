/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/stream.h"
#include "ove/storage.h"
#include "FreeRTOS.h"
#include "ove_ns_to_ticks.h"
#include "semphr.h"
#include "task.h"
#include <stdbool.h>

/*
 * Backed by a plain ring buffer (ove/ring.h) rather than a FreeRTOS
 * StreamBuffer: the stream API is trigger-aware (a receive blocks until at
 * least `trigger` bytes are present, see ove/stream.h) but a StreamBuffer's
 * trigger only
 * gates when the *sender* wakes a blocked reader — the reader still returns
 * whatever sub-trigger data is present. The ring is guarded by a critical
 * section (mutual exclusion between thread and ISR context) and two counting
 * semaphores provide the blocking wait/wake. Sem counts are only hints:
 * every waiter re-checks the ring under the lock after waking (a blocked
 * waiter always left its sem at 0, so the next give wakes it).
 */

/* ── _init / _deinit ────────────────────────────────────────────────── */

int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger)
{
	if (stream == NULL || storage == NULL || buffer == NULL || size == 0)
		return OVE_ERR_INVALID_PARAM;

	ove_ring_setup(&storage->ring, (unsigned char *)buffer, size);
	storage->trigger = (trigger > 0) ? trigger : 1; /* 0 is treated as 1 */
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
		if (stream->ring.count < stream->ring.size) {
			written += ove_ring_write(&stream->ring, src + written, len - written);
			have_trigger = stream->ring.count >= stream->trigger;
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
		if (stream->ring.count >= stream->trigger) {
			received = ove_ring_read(&stream->ring, (unsigned char *)buf, len);
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
	size_t written = ove_ring_write(&stream->ring, (const unsigned char *)data, len);
	bool have_trigger = stream->ring.count >= stream->trigger;
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
	size_t received = ove_ring_read(&stream->ring, (unsigned char *)buf, len);
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
	ove_ring_clear(&stream->ring);
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
	size_t count = stream->ring.count;
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
