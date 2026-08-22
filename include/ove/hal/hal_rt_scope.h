/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Physical board contract for the Linux-personality real-time scope probe.
 */
#ifndef OVE_HAL_RT_SCOPE_H
#define OVE_HAL_RT_SCOPE_H

#include <stdint.h>

#include "ove/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OVE_RT_SCOPE_TICKS_PER_US 54u
#define OVE_RT_SCOPE_PERIOD_US 1000u
#define OVE_RT_SCOPE_PERIOD_TICKS (OVE_RT_SCOPE_PERIOD_US * OVE_RT_SCOPE_TICKS_PER_US)

typedef void (*ove_hal_rt_scope_release_fn)(void);

int ove_hal_rt_scope_irq_prepare(ove_hal_rt_scope_release_fn release);
void ove_hal_rt_scope_hardware_prepare(void);
void ove_hal_rt_scope_start(void);
void ove_hal_rt_scope_irq_ack(void);

uint32_t ove_hal_rt_scope_phase_ticks(void);
uint32_t ove_hal_rt_scope_time_us(void);
void ove_hal_rt_scope_response_set(int high);

int ove_hal_rt_scope_release_attribution(uint32_t *preempt_locked, uint32_t *owner_pid);
int ove_hal_rt_scope_release_attribution_available(void);

#ifdef __cplusplus
}
#endif

#endif /* OVE_HAL_RT_SCOPE_H */
