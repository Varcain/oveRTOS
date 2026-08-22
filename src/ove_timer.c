/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/timer.h"

#ifdef CONFIG_OVE_TIMER

int ove_timer_init(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		   void *user_data, uint32_t period_ms, int one_shot)
{
	return ove_timer_init_ns(timer, storage, callback, user_data,
				 (uint64_t)period_ms * 1000000ULL, one_shot);
}

#ifdef OVE_HEAP_TIMER

#include "ove_backend_common.h"

int ove_timer_create_ns(ove_timer_t *timer, ove_timer_fn callback, void *user_data,
			uint64_t period_ns, int one_shot)
{
	int rc = ove_check_param(timer);
	if (rc)
		return rc;
	if (!callback)
		return OVE_ERR_INVALID_PARAM;

	ove_timer_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	rc = ove_timer_init_ns(timer, storage, callback, user_data, period_ns, one_shot);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

int ove_timer_create(ove_timer_t *timer, ove_timer_fn callback, void *user_data, uint32_t period_ms,
		     int one_shot)
{
	return ove_timer_create_ns(timer, callback, user_data, (uint64_t)period_ms * 1000000ULL,
				   one_shot);
}

void ove_timer_destroy(ove_timer_t timer)
{
	if (timer) {
		ove_timer_deinit(timer);
		OVE_BACKEND_FREE(timer);
	}
}

#endif
#endif
