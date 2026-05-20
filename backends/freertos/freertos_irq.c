/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * FreeRTOS backend for ove/irq.h.
 *
 * Critical sections dispatch between the thread-context macros
 * (`taskENTER_CRITICAL` / `taskEXIT_CRITICAL`) and the ISR-context
 * macros (`taskENTER_CRITICAL_FROM_ISR` / `taskEXIT_CRITICAL_FROM_ISR`)
 * based on `xPortIsInsideInterrupt()`. The ISR variants return /
 * consume a saved interrupt mask (`UBaseType_t`); the thread variants
 * carry no cookie. We pack both cases into a uint64_t cookie:
 *
 *   bit 63   : 1 if the lock was taken from ISR context, 0 otherwise.
 *   bits 0-31: saved interrupt mask (only meaningful when bit 63 is set).
 *
 * This lets `ove_irq_unlock` recover the original context without the
 * caller needing to know.
 *
 * Available only when CONFIG_OVE_ASYNC is set in the C config.
 */

#include "ove/irq.h"
#include "ove/types.h"
#include "ove_config.h"

#ifdef CONFIG_OVE_ASYNC

#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include <stdint.h>

#define OVE_IRQ_KEY_FROM_ISR_BIT ((uint64_t)1ULL << 63)

ove_irq_key_t ove_irq_lock(void)
{
	if (xPortIsInsideInterrupt()) {
		UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
		return OVE_IRQ_KEY_FROM_ISR_BIT | (uint64_t)saved;
	}
	taskENTER_CRITICAL();
	return 0;
}

void ove_irq_unlock(ove_irq_key_t key)
{
	if (key & OVE_IRQ_KEY_FROM_ISR_BIT) {
		taskEXIT_CRITICAL_FROM_ISR((UBaseType_t)(key & 0xFFFFFFFFULL));
	} else {
		taskEXIT_CRITICAL();
	}
}

bool ove_is_in_isr(void)
{
	return xPortIsInsideInterrupt() != 0;
}

#endif /* CONFIG_OVE_ASYNC */
