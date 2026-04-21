/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

#include "ove/trace.h"
#include "ove/thread_state_stats.h"
#include "ove_trace_ring.h"

void ove_trace_emit_state(uintptr_t thread_handle, int old_state, int new_state)
{
	struct ove_trace_record rec = {
		.ts_us = ove_state_stats_now_us(),
		.tid   = (uint32_t)thread_handle,
		.kind  = OVE_TRACE_KIND_STATE,
		.code  = (uint8_t)new_state,
		.arg   = (uint16_t)old_state,
	};
	(void)ove_trace_ring_push(&rec);
}

void ove_trace_emit_mark(uintptr_t thread_handle,
			 uint8_t prim, uint8_t act, uintptr_t object)
{
	struct ove_trace_record rec = {
		.ts_us = ove_state_stats_now_us(),
		.tid   = (uint32_t)thread_handle,
		.kind  = OVE_TRACE_KIND_MARK,
		.code  = (uint8_t)((prim << 4) | (act & 0x0F)),
		.arg   = (uint16_t)(object & 0xFFFFu),
	};
	(void)ove_trace_ring_push(&rec);
}

#endif /* CONFIG_OVE_TRACE_STREAM */
