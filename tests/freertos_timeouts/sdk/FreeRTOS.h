/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
/* Scalar arithmetic models only: no native port, scheduler or task is linked. */
#if OVE_TICK_BITS == 16
typedef uint16_t TickType_t;
#define portMAX_DELAY UINT16_MAX
#elif OVE_TICK_BITS == 32
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#elif OVE_TICK_BITS == 64
typedef uint64_t TickType_t;
#define portMAX_DELAY UINT64_MAX
#else
#error "unsupported arithmetic model width"
#endif
#define configTICK_RATE_HZ ((uint64_t)OVE_TICK_RATE)
