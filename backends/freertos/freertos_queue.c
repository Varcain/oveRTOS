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
#include "queue.h"

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *storage,
		       void *buffer, size_t item_size, unsigned int max_items)
{
	if (q == NULL || storage == NULL || buffer == NULL ||
	    item_size == 0 || max_items == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->storage = (uint8_t *)buffer;
	storage->queue = xQueueCreateStatic(max_items, item_size,
					    storage->storage,
					    &storage->static_queue);
	*q = storage;
	return OVE_OK;
}

void ove_queue_deinit(ove_queue_t q)
{
	if (q != NULL) {
		vQueueDelete(q->queue);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_QUEUE
int ove_queue_create(ove_queue_t *q, size_t item_size,
				 unsigned int max_items)
{
	struct ove_queue *w;
	size_t storage_size;

	if (q == NULL || item_size == 0 || max_items == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (w == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	storage_size = (size_t)max_items * item_size;
	w->storage = OVE_BACKEND_MALLOC(storage_size);
	if (w->storage == NULL) {
		OVE_BACKEND_FREE(w);
		return OVE_ERR_NO_MEMORY;
	}

	w->queue = xQueueCreateStatic(max_items, item_size,
				      w->storage, &w->static_queue);

	*q = w;
	return OVE_OK;
}

void ove_queue_destroy(ove_queue_t q)
{
	if (q != NULL) {
		vQueueDelete(q->queue);
		OVE_BACKEND_FREE(q->storage);
		OVE_BACKEND_FREE(q);
	}
}
#endif /* OVE_HEAP_QUEUE */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_queue_send(ove_queue_t q, const void *data,
			       uint32_t timeout_ms)
{
	TickType_t ticks;
	BaseType_t ret;

	configASSERT(q != NULL);
	if (timeout_ms == OVE_WAIT_FOREVER) {
		ticks = portMAX_DELAY;
	} else {
		ticks = pdMS_TO_TICKS(timeout_ms);
	}

	ret = xQueueSend(q->queue, data, ticks);
	if (ret != pdPASS) {
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_queue_receive(ove_queue_t q, void *buf,
				  uint32_t timeout_ms)
{
	TickType_t ticks;
	BaseType_t ret;

	configASSERT(q != NULL);
	if (timeout_ms == OVE_WAIT_FOREVER) {
		ticks = portMAX_DELAY;
	} else {
		ticks = pdMS_TO_TICKS(timeout_ms);
	}

	ret = xQueueReceive(q->queue, buf, ticks);
	if (ret != pdPASS) {
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_queue_send_from_isr(ove_queue_t q, const void *data)
{
	BaseType_t yield_required = pdFALSE;
	BaseType_t ret;

	configASSERT(q != NULL);
	ret = xQueueSendFromISR(q->queue, data, &yield_required);
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

	configASSERT(q != NULL);
	ret = xQueueReceiveFromISR(q->queue, buf, &yield_required);
	portYIELD_FROM_ISR(yield_required);

	if (ret != pdPASS) {
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}
