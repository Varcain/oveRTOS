/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * NuttX backend for ove/irq.h.
 *
 * NuttX's enter_critical_section / leave_critical_section work from
 * both thread and ISR context — the kernel handles dispatch internally
 * via irqsave/irqrestore on ARM, plus extra bookkeeping for SMP. The
 * returned irqstate_t is widened to ove_irq_key_t (uint64_t) for ABI
 * consistency across backends.
 *
 * Available only when CONFIG_OVE_ASYNC is set in the oveRTOS config.
 */

#include "ove/irq.h"
#include "ove/types.h"
#include "ove_config.h"

#ifdef CONFIG_OVE_ASYNC

#include <nuttx/irq.h>
#include <stdbool.h>
#include <stdint.h>

ove_irq_key_t ove_irq_lock(void)
{
	return (ove_irq_key_t)enter_critical_section();
}

void ove_irq_unlock(ove_irq_key_t key)
{
	leave_critical_section((irqstate_t)key);
}

bool ove_is_in_isr(void)
{
	return up_interrupt_context();
}

#endif /* CONFIG_OVE_ASYNC */
