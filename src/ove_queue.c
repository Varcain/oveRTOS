/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/queue.h"

#if defined(CONFIG_OVE_QUEUE) && defined(OVE_HEAP_QUEUE)

#include "ove_backend_common.h"

int ove_queue_create(ove_queue_t *q, size_t item_size, unsigned int max_items)
{
	int rc = ove_check_param(q);
	if (rc)
		return rc;
	if (!item_size || !max_items || item_size > SIZE_MAX / max_items)
		return OVE_ERR_INVALID_PARAM;

	size_t buffer_size = item_size * (size_t)max_items;
	if (buffer_size > SIZE_MAX - sizeof(ove_queue_storage_t))
		return OVE_ERR_INVALID_PARAM;
	ove_queue_storage_t *storage =
		OVE_BACKEND_MALLOC(sizeof(*storage) + buffer_size);
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	void *buffer = storage + 1;
	rc = ove_queue_init(q, storage, buffer, item_size, max_items);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_queue_destroy(ove_queue_t q)
{
	if (q) {
		ove_queue_deinit(q);
		OVE_BACKEND_FREE(q);
	}
}

#endif
