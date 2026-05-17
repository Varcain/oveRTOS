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
#include <zephyr/kernel.h>
/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_eventgroup_init(ove_eventgroup_t *eg, ove_eventgroup_storage_t *storage)
{
	if (eg == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	k_event_init(&storage->event);
	*eg = storage;
	return OVE_OK;
}

void ove_eventgroup_deinit(ove_eventgroup_t eg)
{
	(void)eg;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_EVENTGROUP
int ove_eventgroup_create(ove_eventgroup_t *eg)
{
	if (eg == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_eventgroup *e = OVE_BACKEND_MALLOC(sizeof(*e));
	if (e == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	int ret = ove_eventgroup_init(eg, e);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(e);
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
	k_event_post(&eg->event, bits);
	return k_event_test(&eg->event, 0xFFFFFFFFU);
}

ove_eventbits_t ove_eventgroup_clear_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	ove_eventbits_t prev = k_event_test(&eg->event, 0xFFFFFFFFU);
	k_event_clear(&eg->event, bits);
	return prev;
}

int ove_eventgroup_wait_bits(ove_eventgroup_t eg, ove_eventbits_t bits, uint32_t flags,
			     uint64_t timeout_ns, ove_eventbits_t *result)
{
	k_timeout_t timeout;
	uint32_t val;
	int wait_all = (flags & OVE_EG_WAIT_ALL) ? 1 : 0;
	int clear = (flags & OVE_EG_CLEAR_ON_EXIT) ? 1 : 0;

	if (timeout_ns == OVE_WAIT_FOREVER) {
		timeout = K_FOREVER;
	} else {
		timeout = K_NSEC(timeout_ns);
	}

	/*
	 * Zephyr's k_event_wait/wait_all 'reset' parameter resets ALL events
	 * to zero BEFORE waiting — not what OVE_EG_CLEAR_ON_EXIT wants.
	 * Use the _safe variants which clear only matched bits ON EXIT.
	 */
	if (wait_all && clear) {
		val = k_event_wait_all_safe(&eg->event, bits, false, timeout);
	} else if (wait_all) {
		val = k_event_wait_all(&eg->event, bits, false, timeout);
	} else if (clear) {
		val = k_event_wait_safe(&eg->event, bits, false, timeout);
	} else {
		val = k_event_wait(&eg->event, bits, false, timeout);
	}

	if (result != NULL) {
		*result = (ove_eventbits_t)val;
	}

	if (val == 0) {
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

ove_eventbits_t ove_eventgroup_set_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	k_event_post(&eg->event, bits);
	return k_event_test(&eg->event, 0xFFFFFFFFU);
}

ove_eventbits_t ove_eventgroup_get_bits(ove_eventgroup_t eg)
{
	return (ove_eventbits_t)k_event_test(&eg->event, 0xFFFFFFFFU);
}
