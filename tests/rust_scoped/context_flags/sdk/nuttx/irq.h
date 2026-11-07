/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "model.h"
typedef uint64_t irqstate_t;
bool up_interrupt_context(void);
irqstate_t up_irq_flags(void);
#define enter_critical_section() model_effect()
#define leave_critical_section(key) ((void)(key), (void)model_effect())
