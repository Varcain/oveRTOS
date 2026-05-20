/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/eventgroup.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
#include "ove_ns_to_ticks.h"
#include "event_groups.h"

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_eventgroup_init(ove_eventgroup_t *eg, ove_eventgroup_storage_t *storage)
{
	if (eg == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	storage->handle = xEventGroupCreateStatic(&storage->static_eg);
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	*eg = storage;
	return OVE_OK;
}

void ove_eventgroup_deinit(ove_eventgroup_t eg)
{
	if (eg != NULL) {
		vEventGroupDelete(eg->handle);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_EVENTGROUP
int ove_eventgroup_create(ove_eventgroup_t *eg)
{
	if (eg == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_eventgroup *w = OVE_BACKEND_MALLOC(sizeof(*w));
	if (w == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	int ret = ove_eventgroup_init(eg, w);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(w);
	}
	return ret;
}

void ove_eventgroup_destroy(ove_eventgroup_t eg)
{
	if (eg != NULL) {
		ove_eventgroup_deinit(eg);
		OVE_BACKEND_FREE(eg);
	}
}
#endif /* OVE_HEAP_EVENTGROUP */

/* ─── Operations ─────────────────────────────────────────────────────── */

ove_eventbits_t ove_eventgroup_set_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	ove_eventbits_t result = (ove_eventbits_t)xEventGroupSetBits(eg->handle, (EventBits_t)bits);
	if (bits != 0 && eg->notify_cb != NULL) {
		eg->notify_cb(eg->notify_ud);
	}
	return result;
}

ove_eventbits_t ove_eventgroup_clear_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	return (ove_eventbits_t)xEventGroupClearBits(eg->handle, (EventBits_t)bits);
}

int ove_eventgroup_wait_bits(ove_eventgroup_t eg, ove_eventbits_t bits, uint32_t flags,
			     uint64_t timeout_ns, ove_eventbits_t *result)
{
	BaseType_t wait_all = (flags & OVE_EG_WAIT_ALL) ? pdTRUE : pdFALSE;
	BaseType_t clear = (flags & OVE_EG_CLEAR_ON_EXIT) ? pdTRUE : pdFALSE;
	TickType_t ticks;
	EventBits_t val;

	if (timeout_ns == OVE_WAIT_FOREVER) {
		ticks = portMAX_DELAY;
	} else {
		ticks = ove_ns_to_ticks(timeout_ns);
	}

	val = xEventGroupWaitBits(eg->handle, (EventBits_t)bits, clear, wait_all, ticks);

	if (result != NULL) {
		*result = (ove_eventbits_t)val;
	}

	if (wait_all) {
		if ((val & bits) == bits) {
			return OVE_OK;
		}
	} else {
		if (val & bits) {
			return OVE_OK;
		}
	}
	return OVE_ERR_TIMEOUT;
}

ove_eventbits_t ove_eventgroup_set_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	BaseType_t yield = pdFALSE;
	BaseType_t ret;

	ret = xEventGroupSetBitsFromISR(eg->handle, (EventBits_t)bits, &yield);
	if (ret == pdPASS && bits != 0 && eg->notify_cb != NULL) {
		eg->notify_cb(eg->notify_ud);
	}
	portYIELD_FROM_ISR(yield);

	if (ret == pdPASS) {
		return bits;
	}
	return 0;
}

ove_eventbits_t ove_eventgroup_get_bits(ove_eventgroup_t eg)
{
	return (ove_eventbits_t)xEventGroupGetBits(eg->handle);
}

int ove_eventgroup_set_notify(ove_eventgroup_t eg, ove_notify_cb cb, void *user_data)
{
	if (eg == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	eg->notify_cb = cb;
	eg->notify_ud = user_data;
	return OVE_OK;
}
