/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../../freertos_event_waiters/mask_sdk/ove_event_mask_ops.h"
#if !OVE_MODEL_UNSUPPORTED_ARCH
#define OVE_IRQ_MASK_CORTEX_M 1
#endif
uint32_t irq_mask_exception(void);
uint32_t irq_mask_control(void);
uint32_t irq_mask_group(void);
uint32_t irq_mask_priority(uint32_t);
_Noreturn void irq_mask_reject(void);
