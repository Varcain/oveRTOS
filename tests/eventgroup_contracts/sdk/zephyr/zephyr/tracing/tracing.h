/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#ifdef OVE_MODEL_EVENT_TRACE
void ove_model_event_trace_enter(struct k_event *, uint32_t, uint32_t);
void ove_model_event_trace_exit(struct k_event *, uint32_t, uint32_t);
#define SYS_PORT_TRACING_OBJ_FUNC_ENTER(type, op, event, value, mask) \
	ove_model_event_trace_enter(event, value, mask)
#define SYS_PORT_TRACING_OBJ_FUNC_EXIT(type, op, event, value, mask) \
	ove_model_event_trace_exit(event, value, mask)
#else
#define SYS_PORT_TRACING_OBJ_FUNC_ENTER(type, op, event, value, mask) ((void)0)
#define SYS_PORT_TRACING_OBJ_FUNC_EXIT(type, op, event, value, mask) ((void)0)
#endif
