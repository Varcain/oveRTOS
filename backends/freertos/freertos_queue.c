/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/queue.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
#include "ove_ns_to_ticks.h"
#include "queue.h"

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *storage, void *buffer, size_t item_size,
		   unsigned int max_items)
{
	if (item_size == 0 || max_items == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->storage = (uint8_t *)buffer;
	storage->queue =
		xQueueCreateStatic(max_items, item_size, storage->storage, &storage->static_queue);
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	*q = storage;
	return OVE_OK;
}

void ove_queue_deinit(ove_queue_t q)
{
	if (q != NULL) {
		vQueueDelete(q->queue);
	}
}

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_queue_send(ove_queue_t q, const void *data, uint64_t timeout_ns)
{
	TickType_t ticks;
	BaseType_t ret;

	if (timeout_ns == OVE_WAIT_FOREVER) {
		ticks = portMAX_DELAY;
	} else {
		ticks = ove_ns_to_ticks(timeout_ns);
	}

	ret = xQueueSend(q->queue, data, ticks);
	if (ret != pdPASS) {
		return (timeout_ns == 0) ? OVE_ERR_QUEUE_FULL : OVE_ERR_TIMEOUT;
	}
	if (q->notify_cb != NULL) {
		q->notify_cb(q->notify_ud);
	}
	return OVE_OK;
}

int ove_queue_receive(ove_queue_t q, void *buf, uint64_t timeout_ns)
{
	TickType_t ticks;
	BaseType_t ret;

	if (timeout_ns == OVE_WAIT_FOREVER) {
		ticks = portMAX_DELAY;
	} else {
		ticks = ove_ns_to_ticks(timeout_ns);
	}

	ret = xQueueReceive(q->queue, buf, ticks);
	if (ret != pdPASS) {
		return (timeout_ns == 0) ? OVE_ERR_QUEUE_EMPTY : OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_queue_send_from_isr(ove_queue_t q, const void *data)
{
	BaseType_t yield_required = pdFALSE;
	BaseType_t ret;

	ret = xQueueSendFromISR(q->queue, data, &yield_required);
	if (ret == pdPASS && q->notify_cb != NULL) {
		q->notify_cb(q->notify_ud);
	}
	portYIELD_FROM_ISR(yield_required);

	if (ret != pdPASS) {
		return OVE_ERR_QUEUE_FULL;
	}
	return OVE_OK;
}

int ove_queue_receive_from_isr(ove_queue_t q, void *buf)
{
	BaseType_t yield_required = pdFALSE;
	BaseType_t ret;

	ret = xQueueReceiveFromISR(q->queue, buf, &yield_required);
	portYIELD_FROM_ISR(yield_required);

	if (ret != pdPASS) {
		return OVE_ERR_QUEUE_EMPTY;
	}
	return OVE_OK;
}

int ove_queue_set_notify(ove_queue_t q, ove_notify_cb cb, void *user_data)
{
	/* OVE_NONNULL(1) on the public decl already guarantees q != NULL;
	 * an explicit check would trip -Werror=nonnull-compare. */
	q->notify_cb = cb;
	q->notify_ud = user_data;
	return OVE_OK;
}
