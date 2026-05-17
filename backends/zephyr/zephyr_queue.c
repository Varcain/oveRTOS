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
#include <zephyr/kernel.h>
#include <string.h>
/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *storage, void *buffer, size_t item_size,
		   unsigned int max_items)
{
	if (q == NULL || storage == NULL || buffer == NULL || item_size == 0 || max_items == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->buffer = (char *)buffer;
	k_msgq_init(&storage->msgq, storage->buffer, item_size, max_items);

	*q = storage;
	return OVE_OK;
}

void ove_queue_deinit(ove_queue_t q)
{
	if (q != NULL) {
		k_msgq_purge(&q->msgq);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_QUEUE
int ove_queue_create(ove_queue_t *q, size_t item_size, unsigned int max_items)
{
	struct ove_queue *zq;

	if (q == NULL || item_size == 0 || max_items == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	zq = OVE_BACKEND_MALLOC(sizeof(*zq));
	if (zq == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	zq->buffer = OVE_BACKEND_MALLOC(item_size * max_items);
	if (zq->buffer == NULL) {
		OVE_BACKEND_FREE(zq);
		return OVE_ERR_NO_MEMORY;
	}

	k_msgq_init(&zq->msgq, zq->buffer, item_size, max_items);

	*q = zq;
	return OVE_OK;
}

void ove_queue_destroy(ove_queue_t q)
{
	if (q != NULL) {
		k_msgq_purge(&q->msgq);
		OVE_BACKEND_FREE(q->buffer);
		OVE_BACKEND_FREE(q);
	}
}
#endif /* OVE_HEAP_QUEUE */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_queue_send(ove_queue_t q, const void *data, uint64_t timeout_ns)
{
	k_timeout_t timeout;
	int ret;

	__ASSERT(q != NULL, "NULL queue handle");
	if (timeout_ns == OVE_WAIT_FOREVER) {
		timeout = K_FOREVER;
	} else {
		timeout = K_NSEC(timeout_ns);
	}

	ret = k_msgq_put(&q->msgq, data, timeout);
	if (ret != 0) {
		return (timeout_ns == 0) ? OVE_ERR_QUEUE_FULL : OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_queue_receive(ove_queue_t q, void *buf, uint64_t timeout_ns)
{
	k_timeout_t timeout;
	int ret;

	__ASSERT(q != NULL, "NULL queue handle");
	if (timeout_ns == OVE_WAIT_FOREVER) {
		timeout = K_FOREVER;
	} else {
		timeout = K_NSEC(timeout_ns);
	}

	ret = k_msgq_get(&q->msgq, buf, timeout);
	if (ret != 0) {
		return (timeout_ns == 0) ? OVE_ERR_QUEUE_EMPTY : OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_queue_send_from_isr(ove_queue_t q, const void *data)
{
	__ASSERT(q != NULL, "NULL queue handle");
	int ret = k_msgq_put(&q->msgq, data, K_NO_WAIT);
	if (ret != 0) {
		return OVE_ERR_QUEUE_FULL;
	}
	return OVE_OK;
}

int ove_queue_receive_from_isr(ove_queue_t q, void *buf)
{
	__ASSERT(q != NULL, "NULL queue handle");
	int ret = k_msgq_get(&q->msgq, buf, K_NO_WAIT);
	if (ret != 0) {
		return OVE_ERR_QUEUE_EMPTY;
	}
	return OVE_OK;
}
