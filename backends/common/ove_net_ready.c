/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ove_net_ready.h"

#include "ove/types.h"

static ove_net_ready_callback_t g_callback;

int ove_net_ready_subscribe(ove_net_ready_callback_t callback)
{
	if (!callback)
		return OVE_ERR_INVALID_PARAM;

	ove_net_ready_callback_t expected = NULL;
	if (__atomic_compare_exchange_n(&g_callback, &expected, callback, 0, __ATOMIC_RELEASE,
					__ATOMIC_ACQUIRE) ||
	    expected == callback)
		return OVE_OK;
	return OVE_ERR_WOULD_BLOCK;
}

void ove_net_ready_unsubscribe(ove_net_ready_callback_t callback)
{
	if (!callback)
		return;
	ove_net_ready_callback_t expected = callback;
	(void)__atomic_compare_exchange_n(&g_callback, &expected, NULL, 0, __ATOMIC_ACQ_REL,
					  __ATOMIC_ACQUIRE);
}

void ove_net_ready_publish(void)
{
	ove_net_ready_callback_t callback = __atomic_load_n(&g_callback, __ATOMIC_ACQUIRE);
	if (callback)
		callback();
}
