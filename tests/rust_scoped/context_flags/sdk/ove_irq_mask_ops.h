/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Host stand-ins for the Cortex-M mask registers; the context model never
 * calls ove_irq_lock or ove_irq_unlock. */
#include <stdint.h>
#define OVE_IRQ_MASK_CORTEX_M 1
static inline _Noreturn void irq_mask_reject(void)
{
	for (;;) {
	}
}
static inline uint32_t irq_mask_exception(void)
{
	return model.isr;
}
static inline uint32_t irq_mask_control(void)
{
	return model.control;
}
static inline uint32_t irq_mask_group(void)
{
	return 0;
}
static inline uint32_t irq_mask_priority(uint32_t exception)
{
	(void)exception;
	return 255;
}
#define event_mask_primask model_primask
#define event_mask_basepri model_basepri
#define event_mask_faultmask model_faultmask
static inline void event_mask_disable_faults(void)
{
}
static inline void event_mask_disable_irqs(void)
{
}
static inline void event_mask_restore_basepri(uint32_t value)
{
	(void)value;
}
static inline void event_mask_restore_primask(uint32_t value)
{
	(void)value;
}
static inline void event_mask_restore_faultmask(uint32_t value)
{
	(void)value;
}
