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
 * Context detection is available with SYNC, ASYNC or QUEUE; critical sections
 * require ASYNC.
 */

#include "ove/irq.h"
#include "ove/types.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_ASYNC) || defined(CONFIG_OVE_QUEUE)

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>
#include "ove_sync_context.h"

#if (defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_QUEUE)) && defined(CONFIG_MULTITHREADING)
#ifndef CONFIG_SMP
extern struct k_thread z_idle_threads[];
#endif
#if defined(CONFIG_BOARD_NATIVE_SIM)
extern uint32_t hw_irq_ctrl_get_current_lock(void);
#endif
#endif

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_QUEUE)
uint32_t ove_sync_context(void)
{
#if defined(CONFIG_MULTITHREADING) && \
	(defined(OVE_SYNC_CORTEX_M) || defined(CONFIG_BOARD_NATIVE_SIM))
	if (k_is_user_context() || k_is_pre_kernel() || k_is_in_isr())
		return 0;
#ifdef OVE_SYNC_CORTEX_M
	if (!ove_sync_privileged())
		return 0;
	bool blocking = ove_sync_irqs_enabled();
#else
	bool blocking = hw_irq_ctrl_get_current_lock() == 0;
#endif
	struct k_thread *current = k_current_get();
	if (current == NULL)
		return 0;
#ifdef CONFIG_SMP
	if (current->base.is_idle)
#else
	if (current == &z_idle_threads[0])
#endif
		return 0;
	return OVE_SYNC_CONTEXT_PRODUCER |
	       (blocking ? OVE_SYNC_CONTEXT_RESOURCE | OVE_SYNC_CONTEXT_BLOCKING : 0u);
#else
	return 0;
#endif
}
#endif

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

#endif /* CONFIG_OVE_SYNC || CONFIG_OVE_ASYNC || CONFIG_OVE_QUEUE */
