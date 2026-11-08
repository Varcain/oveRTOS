/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#define CONFIG_OVE_RTOS_FREERTOS 1
#define CONFIG_OVE_SYNC 1
#define CONFIG_OVE_EVENTGROUP 1
#if OVE_NATIVE_TRACE
#define CONFIG_OVE_THREAD_STATE_STATS 1
#define CONFIG_OVE_TRACE_STREAM 1
#define CONFIG_OVE_TRACE_MARKERS 1
#define CONFIG_OVE_TRACE_RING_SIZE 64
#endif
