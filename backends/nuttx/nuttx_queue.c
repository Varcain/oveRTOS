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
#include "ove_ns_to_ticks.h"
#include <stdint.h>
#include <string.h>
#include <errno.h>

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *storage, void *buffer, size_t item_size,
		   unsigned int max_items)
{
	if (q == NULL || storage == NULL || buffer == NULL || item_size == 0 || max_items == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->buffer = buffer;
	storage->item_size = item_size;
	storage->max_items = max_items;
	storage->head = 0;
	storage->tail = 0;
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
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

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_queue_send(ove_queue_t q, const void *data, uint64_t timeout_ns)
{
	DEBUGASSERT(q != NULL);
	struct ove_queue *nq = q;
	irqstate_t flags;
	int ret;

	if (timeout_ns == OVE_WAIT_FOREVER) {
		ret = nxsem_wait_uninterruptible(&nq->not_full);
	} else {
		ret = nxsem_tickwait_uninterruptible(&nq->not_full, ove_ns_to_ticks(timeout_ns));
	}
	if (ret < 0) {
		return (timeout_ns == 0) ? OVE_ERR_QUEUE_FULL : OVE_ERR_TIMEOUT;
	}

	flags = enter_critical_section();
	memcpy((char *)nq->buffer + nq->head * nq->item_size, data, nq->item_size);
	nq->head = (nq->head + 1) % nq->max_items;
	leave_critical_section(flags);

	nxsem_post(&nq->not_empty);
	if (nq->notify_cb != NULL) {
		nq->notify_cb(nq->notify_ud);
	}
	return OVE_OK;
}

int ove_queue_receive(ove_queue_t q, void *buf, uint64_t timeout_ns)
{
	DEBUGASSERT(q != NULL);
	struct ove_queue *nq = q;
	irqstate_t flags;
	int ret;

	if (timeout_ns == OVE_WAIT_FOREVER) {
		ret = nxsem_wait_uninterruptible(&nq->not_empty);
	} else {
		ret = nxsem_tickwait_uninterruptible(&nq->not_empty, ove_ns_to_ticks(timeout_ns));
	}
	if (ret < 0) {
		return (timeout_ns == 0) ? OVE_ERR_QUEUE_EMPTY : OVE_ERR_TIMEOUT;
	}

	flags = enter_critical_section();
	memcpy(buf, (char *)nq->buffer + nq->tail * nq->item_size, nq->item_size);
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
	memcpy((char *)nq->buffer + nq->head * nq->item_size, data, nq->item_size);
	nq->head = (nq->head + 1) % nq->max_items;

	nxsem_post(&nq->not_empty);
	if (nq->notify_cb != NULL) {
		nq->notify_cb(nq->notify_ud);
	}
	return OVE_OK;
}

int ove_queue_receive_from_isr(ove_queue_t q, void *buf)
{
	DEBUGASSERT(q != NULL);
	struct ove_queue *nq = q;

	if (nxsem_trywait(&nq->not_empty) < 0) {
		return OVE_ERR_QUEUE_EMPTY;
	}

	/* ISR context: implicit exclusion on single-core NuttX */
	memcpy(buf, (char *)nq->buffer + nq->tail * nq->item_size, nq->item_size);
	nq->tail = (nq->tail + 1) % nq->max_items;

	nxsem_post(&nq->not_full);
	return OVE_OK;
}

int ove_queue_set_notify(ove_queue_t q, ove_notify_cb cb, void *user_data)
{
	struct ove_queue *nq = q;
	if (nq == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	irqstate_t flags = enter_critical_section();
	nq->notify_cb = cb;
	nq->notify_ud = user_data;
	leave_critical_section(flags);
	return OVE_OK;
}
