/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <time.h>
/* Explicit arithmetic models; no native clock, scheduler or task is linked. */
#if OVE_CLOCK_BITS == 64
typedef uint64_t ove_model_clock_t;
typedef int64_t sclock_t;
#define CLOCK_MAX UINT64_MAX
#else
typedef uint32_t ove_model_clock_t;
typedef int32_t sclock_t;
#define CLOCK_MAX UINT32_MAX
#endif
#define clock_t ove_model_clock_t
clock_t clock_systime_ticks(void);
#define NSEC_PER_TICK ((uint64_t)OVE_TICK_NS)
#define clock_compare(first, second) ((sclock_t)((second) - (first)) >= 0)
