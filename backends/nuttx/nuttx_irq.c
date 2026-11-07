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
 * Context detection is available with SYNC or ASYNC; critical sections
 * require ASYNC.
 */

#include "ove/irq.h"
#include "ove/types.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_ASYNC)

#include <nuttx/irq.h>
#include <stdbool.h>
#include <stdint.h>
#include "ove_sync_context.h"

#if defined(CONFIG_OVE_SYNC) && defined(CONFIG_BUILD_FLAT) && !defined(CONFIG_SMP) && \
	(defined(OVE_SYNC_CORTEX_M) || defined(CONFIG_ARCH_SIM))
#include <nuttx/init.h>
#include <nuttx/sched.h>
#endif

#ifdef CONFIG_OVE_SYNC
uint32_t ove_sync_context(void)
{
#if defined(CONFIG_BUILD_FLAT) && !defined(CONFIG_SMP) && \
	(defined(OVE_SYNC_CORTEX_M) || defined(CONFIG_ARCH_SIM))
#ifdef OVE_SYNC_CORTEX_M
	if (!ove_sync_privileged() || !ove_sync_irqs_enabled())
		return 0;
#else
	if (up_irq_flags() != 0)
		return 0;
#endif
	if ((g_nx_initstate != OSINIT_OSREADY && g_nx_initstate != OSINIT_IDLELOOP) ||
	    up_interrupt_context())
		return 0;
	struct tcb_s *current = nxsched_self();
	if (current == NULL || current->pid == IDLE_PROCESS_ID)
		return 0;
	return OVE_SYNC_CONTEXT_RESOURCE | OVE_SYNC_CONTEXT_BLOCKING;
#else
	return 0;
#endif
}
#endif

#ifdef CONFIG_OVE_ASYNC

ove_irq_key_t ove_irq_lock(void)
{
	return (ove_irq_key_t)enter_critical_section();
}

void ove_irq_unlock(ove_irq_key_t key)
{
	leave_critical_section((irqstate_t)key);
}

#endif /* CONFIG_OVE_ASYNC */

bool ove_is_in_isr(void)
{
	return up_interrupt_context();
}

#endif /* CONFIG_OVE_SYNC || CONFIG_OVE_ASYNC */
