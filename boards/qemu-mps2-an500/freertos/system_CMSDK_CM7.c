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

#define CMSDK_SYSCLK_HZ 25000000u

/* The unit-test builds compile this file against a stub board description. */
#include "board_desc.h"
#ifdef OVE_CLOCK_SYSCLK_MHZ
_Static_assert(CMSDK_SYSCLK_HZ == OVE_CLOCK_SYSCLK_MHZ * 1000000ul,
	       "board.yaml sysclk differs from the MPS2-AN500 core clock");
#endif

uint32_t SystemCoreClock = CMSDK_SYSCLK_HZ;

void SystemInit(void)
{
	/* QEMU MPS2-AN500 needs no special init — PLL, flash wait states, etc.
	 * are not modeled. Just ensure SystemCoreClock is set. */
	SystemCoreClock = CMSDK_SYSCLK_HZ;
}

void SystemCoreClockUpdate(void)
{
	SystemCoreClock = CMSDK_SYSCLK_HZ;
}
