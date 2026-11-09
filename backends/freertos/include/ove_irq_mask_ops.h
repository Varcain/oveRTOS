/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_IRQ_MASK_OPS_H
#define OVE_IRQ_MASK_OPS_H
#include <stdint.h>
#include <ove_event_mask_ops.h>

/* Rejection needs no assertions, allocator, libc, clock or RTOS service. */
static inline _Noreturn void irq_mask_reject(void)
{
	for (;;)
		__asm__ volatile("" : : : "memory");
}
#ifdef OVE_EVENT_MASK_CORTEX_M
#define OVE_IRQ_MASK_CORTEX_M 1
static inline uint32_t irq_mask_exception(void)
{
	uint32_t value;
	__asm__ volatile("mrs %0, ipsr" : "=r"(value) : : "memory");
	return value & 0x1ffu;
}
static inline uint32_t irq_mask_control(void)
{
	uint32_t value;
	__asm__ volatile("mrs %0, control" : "=r"(value) : : "memory");
	return value;
}
static inline uint32_t irq_mask_group(void)
{
	return (*(const volatile uint32_t *)0xe000ed0cu >> 8) & 7u;
}
static inline uint32_t irq_mask_priority(uint32_t exception)
{
	uintptr_t address = exception >= 16u ? 0xe000e400u + exception - 16u
					     : 0xe000ed18u + exception - 4u;
	return *(const volatile uint8_t *)address;
}
#endif
#endif
