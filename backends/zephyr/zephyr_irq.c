/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Zephyr backend for ove/irq.h.
 *
 * Zephyr's irq_lock / irq_unlock work uniformly from thread and ISR
 * context — no dispatch needed. The returned key is an unsigned int
 * that we widen to ove_irq_key_t (uint64_t) for ABI consistency
 * across backends.
 *
 * Context detection is available with SYNC or ASYNC; critical sections
 * require ASYNC.
 */

#include "ove/irq.h"
#include "ove/types.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_ASYNC)

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_OVE_ASYNC
#include <zephyr/irq.h>

ove_irq_key_t ove_irq_lock(void)
{
	return (ove_irq_key_t)irq_lock();
}

void ove_irq_unlock(ove_irq_key_t key)
{
	irq_unlock((unsigned int)key);
}

#endif /* CONFIG_OVE_ASYNC */

bool ove_is_in_isr(void)
{
	return k_is_in_isr();
}

#endif /* CONFIG_OVE_SYNC || CONFIG_OVE_ASYNC */
