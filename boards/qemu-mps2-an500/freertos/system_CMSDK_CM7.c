/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * System initialization for CMSDK Cortex-M7 (QEMU MPS2-AN500).
 * Derived from ARM CMSIS_5 Device pack.
 */

#include <stdint.h>

#define __CM3_REV 0x0201
#define __NVIC_PRIO_BITS 3
#define __Vendor_SysTickConfig 0

uint32_t SystemCoreClock = 25000000u;

void SystemInit(void)
{
	/* QEMU MPS2-AN500 needs no special init — PLL, flash wait states, etc.
	 * are not modeled. Just ensure SystemCoreClock is set. */
	SystemCoreClock = 25000000u;
}

void SystemCoreClockUpdate(void)
{
	SystemCoreClock = 25000000u;
}
