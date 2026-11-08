/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#define OVE_EVENT_MASK_CORTEX_M 1
uint32_t event_mask_primask(void);
uint32_t event_mask_basepri(void);
uint32_t event_mask_faultmask(void);
void event_mask_disable_faults(void);
void event_mask_disable_irqs(void);
void event_mask_restore_basepri(uint32_t);
void event_mask_restore_primask(uint32_t);
void event_mask_restore_faultmask(uint32_t);
