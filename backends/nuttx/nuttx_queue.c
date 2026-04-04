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
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include <string.h>
#include <errno.h>

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *storage,
		       void *buffer, size_t item_size, unsigned int max_items)
{
	if (q == NULL || storage == NULL || buffer == NULL ||
	    item_size == 0 || max_items == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->buffer = buffer;
	storage->item_size = item_size;
	storage->max_items = max_items;
	storage->head = 0;
	storage->tail = 0;
	nxsem_init(&storage->not_full, 0, max_items);
	nxsem_init(&storage->not_empty, 0, 0);

	*q = storage;
	return OVE_OK;
}

void ove_queue_deinit(ove_queue_t q)
{
	if (q != NULL) {
		struct ove_queue *nq = q;
		nxsem_destroy(&nq->not_full);
		nxsem_destroy(&nq->not_empty);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_QUEUE
int ove_queue_create(ove_queue_t *q, size_t item_size,
                              unsigned int max_items)
{
	if (q == NULL || item_size == 0 || max_items == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_queue *nq = OVE_BACKEND_MALLOC(sizeof(*nq));
	if (nq == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(nq, 0, sizeof(*nq));

	nq->buffer = OVE_BACKEND_MALLOC(item_size * max_items);
	if (nq->buffer == NULL) {
		OVE_BACKEND_FREE(nq);
		return OVE_ERR_NO_MEMORY;
	}

	nq->item_size = item_size;
	nq->max_items = max_items;
	nxsem_init(&nq->not_full, 0, max_items);
	nxsem_init(&nq->not_empty, 0, 0);

	*q = nq;
	return OVE_OK;
}

void ove_queue_destroy(ove_queue_t q)
{
	if (q != NULL) {
		struct ove_queue *nq = q;
		nxsem_destroy(&nq->not_full);
		nxsem_destroy(&nq->not_empty);
		OVE_BACKEND_FREE(nq->buffer);
		OVE_BACKEND_FREE(nq);
	}
}
#endif /* OVE_HEAP_QUEUE */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_queue_send(ove_queue_t q, const void *data,
                            uint32_t timeout_ms)
{
	DEBUGASSERT(q != NULL);
	struct ove_queue *nq = q;
	irqstate_t flags;
	int ret;

	if (timeout_ms == OVE_WAIT_FOREVER) {
		ret = nxsem_wait_uninterruptible(&nq->not_full);
	} else {
		ret = nxsem_tickwait_uninterruptible(&nq->not_full,
						      MSEC2TICK(timeout_ms));
	}
	if (ret < 0) {
		return OVE_ERR_TIMEOUT;
	}

	flags = enter_critical_section();
	memcpy((char *)nq->buffer + nq->head * nq->item_size,
	       data, nq->item_size);
	nq->head = (nq->head + 1) % nq->max_items;
	leave_critical_section(flags);

	nxsem_post(&nq->not_empty);
	return OVE_OK;
}

int ove_queue_receive(ove_queue_t q, void *buf,
                               uint32_t timeout_ms)
{
	DEBUGASSERT(q != NULL);
	struct ove_queue *nq = q;
	irqstate_t flags;
	int ret;

	if (timeout_ms == OVE_WAIT_FOREVER) {
		ret = nxsem_wait_uninterruptible(&nq->not_empty);
	} else {
		ret = nxsem_tickwait_uninterruptible(&nq->not_empty,
						      MSEC2TICK(timeout_ms));
	}
	if (ret < 0) {
		return OVE_ERR_TIMEOUT;
	}

	flags = enter_critical_section();
	memcpy(buf, (char *)nq->buffer + nq->tail * nq->item_size,
	       nq->item_size);
	nq->tail = (nq->tail + 1) % nq->max_items;
	leave_critical_section(flags);

	nxsem_post(&nq->not_full);
	return OVE_OK;
}

int ove_queue_send_from_isr(ove_queue_t q, const void *data)
{
	DEBUGASSERT(q != NULL);
	struct ove_queue *nq = q;

	if (nxsem_trywait(&nq->not_full) < 0) {
		return OVE_ERR_QUEUE_FULL;
	}

	/* ISR context: implicit exclusion on single-core NuttX */
	memcpy((char *)nq->buffer + nq->head * nq->item_size,
	       data, nq->item_size);
	nq->head = (nq->head + 1) % nq->max_items;

	nxsem_post(&nq->not_empty);
	return OVE_OK;
}

int ove_queue_receive_from_isr(ove_queue_t q, void *buf)
{
	DEBUGASSERT(q != NULL);
	struct ove_queue *nq = q;

	if (nxsem_trywait(&nq->not_empty) < 0) {
		return OVE_ERR_TIMEOUT;
	}

	/* ISR context: implicit exclusion on single-core NuttX */
	memcpy(buf, (char *)nq->buffer + nq->tail * nq->item_size,
	       nq->item_size);
	nq->tail = (nq->tail + 1) % nq->max_items;

	nxsem_post(&nq->not_full);
	return OVE_OK;
}
