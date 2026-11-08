/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OVE_EVENT_MASK_OPS_H
#define OVE_EVENT_MASK_OPS_H

#include <stdint.h>

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
#define OVE_EVENT_MASK_CORTEX_M 1

static inline uint32_t event_mask_primask(void)
{
	uint32_t value;
	__asm volatile("mrs %0, primask" : "=r"(value) : : "memory");
	return value;
}

static inline uint32_t event_mask_basepri(void)
{
	uint32_t value;
	__asm volatile("mrs %0, basepri" : "=r"(value) : : "memory");
	return value;
}

static inline uint32_t event_mask_faultmask(void)
{
	uint32_t value;
	__asm volatile("mrs %0, faultmask" : "=r"(value) : : "memory");
	return value;
}

static inline void event_mask_disable_faults(void)
{
	__asm volatile("cpsid f\n\tdsb\n\tisb" : : : "memory");
}

static inline void event_mask_disable_irqs(void)
{
	__asm volatile("cpsid i\n\tdsb\n\tisb" : : : "memory");
}

static inline void event_mask_restore_basepri(uint32_t value)
{
	/* PRIMASK remains set across this write, including on Cortex-M7 r0p1. */
	__asm volatile("msr basepri, %0\n\tdsb\n\tisb" : : "r"(value) : "memory");
}

static inline void event_mask_restore_primask(uint32_t value)
{
	__asm volatile("msr primask, %0\n\tisb" : : "r"(value) : "memory");
}

static inline void event_mask_restore_faultmask(uint32_t value)
{
	__asm volatile("msr faultmask, %0\n\tisb" : : "r"(value) : "memory");
}
#endif

#endif /* OVE_EVENT_MASK_OPS_H */
