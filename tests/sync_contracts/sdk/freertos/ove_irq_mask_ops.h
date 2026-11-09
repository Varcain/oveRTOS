/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#define OVE_IRQ_MASK_CORTEX_M 1
uint32_t irq_mask_exception(void);
uint32_t irq_mask_control(void);
uint32_t irq_mask_group(void);
uint32_t irq_mask_priority(uint32_t);
_Noreturn void irq_mask_reject(void);
uint32_t event_mask_primask(void);
uint32_t event_mask_basepri(void);
uint32_t event_mask_faultmask(void);
void event_mask_disable_faults(void);
void event_mask_disable_irqs(void);
void event_mask_restore_basepri(uint32_t);
void event_mask_restore_primask(uint32_t);
void event_mask_restore_faultmask(uint32_t);
