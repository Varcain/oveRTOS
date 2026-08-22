/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/stream.h"

#if defined(CONFIG_OVE_STREAM) && defined(OVE_HEAP_STREAM)

#include "ove_backend_common.h"

int ove_stream_create(ove_stream_t *stream, size_t size, size_t trigger)
{
	int rc = ove_check_param(stream);
	if (rc)
		return rc;
	if (!size || size > SIZE_MAX - sizeof(ove_stream_storage_t))
		return OVE_ERR_INVALID_PARAM;

	ove_stream_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage) + size);
	if (!storage)
		return OVE_ERR_NO_MEMORY;

	void *buffer = storage + 1;
	rc = ove_stream_init(stream, storage, buffer, size, trigger);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_stream_destroy(ove_stream_t stream)
{
	if (stream) {
		ove_stream_deinit(stream);
		OVE_BACKEND_FREE(stream);
	}
}

#endif
