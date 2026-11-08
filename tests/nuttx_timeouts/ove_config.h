/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#define CONFIG_OVE_RTOS_NUTTX 1
#define CONFIG_OVE_QUEUE 1
#define CONFIG_OVE_SYNC 1
#define CONFIG_OVE_EVENTGROUP 1
#define CONFIG_OVE_STREAM 1
#define CONFIG_OVE_TIMER 1
#ifdef OVE_MODEL_ZERO_HEAP
#define CONFIG_OVE_ZERO_HEAP 1
#endif
