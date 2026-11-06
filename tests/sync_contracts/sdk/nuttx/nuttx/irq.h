/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
typedef unsigned irqstate_t;
irqstate_t enter_critical_section(void);
void leave_critical_section(irqstate_t);
bool up_interrupt_context(void);
