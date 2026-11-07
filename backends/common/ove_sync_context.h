/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_SYNC_CONTEXT_H
#define OVE_SYNC_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

/* These are the Cortex-M architectures used by the supported ARM ports. */
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
#define OVE_SYNC_CORTEX_M 1

static inline bool ove_sync_privileged(void)
{
	uint32_t control;
	__asm__ volatile("mrs %0, control" : "=r"(control) : : "memory");
	return (control & 1u) == 0;
}

static inline bool ove_sync_irqs_enabled(void)
{
	uint32_t primask, basepri, faultmask;
	__asm__ volatile("mrs %0, primask" : "=r"(primask) : : "memory");
	__asm__ volatile("mrs %0, basepri" : "=r"(basepri) : : "memory");
	__asm__ volatile("mrs %0, faultmask" : "=r"(faultmask) : : "memory");
	return (primask | basepri | faultmask) == 0;
}
#endif

#endif
