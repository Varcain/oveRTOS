/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Renode-target peripheral observability helpers.
 *
 * The Renode test firmware runs against modelled STM32 peripherals.
 * Every modelled peripheral is memory-mapped at its real STM32F746
 * address — Renode's `STM32_GPIOPort`, `STM32_Timer`, `STM32F4_RTC`,
 * etc. all answer to the same register offsets the HAL writes.  That
 * means a test can verify driver behaviour by reading the peripheral
 * register state directly, bypassing the driver's read-back path and
 * proving the driver's *write* hit the right register.
 *
 * These macros provide the canonical entry points.  Only meaningful on
 * `OVE_RENODE_STM32F746` builds; on other targets they expand to
 * compile-time skips so a single test file can be shared across the
 * whole common-suite list without #ifdef noise per call site.
 */

#ifndef OVE_TEST_RENODE_OBS_H
#define OVE_TEST_RENODE_OBS_H

#include <stdint.h>

#ifdef OVE_RENODE_STM32F746

#include "stm32f7xx_hal.h"

/* GPIO output-data register read for any port.
 *   `port` is the GPIOx pointer from CMSIS (e.g. GPIOA, GPIOI). */
#define OVE_OBS_GPIO_ODR(port) ((port)->ODR)
#define OVE_OBS_GPIO_PIN_HIGH(port, n) (((port)->ODR & (1U << (n))) != 0U)

/* Generic peripheral register read at an absolute address. */
static inline uint32_t ove_obs_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

#define OVE_OBS_AVAILABLE 1

#else /* !OVE_RENODE_STM32F746 */

/* On non-Renode targets these expand to constants that always make the
 * dependent assertion fall into a skip branch — see the
 * OVE_OBS_AVAILABLE gate inside each test. */
#define OVE_OBS_AVAILABLE 0
#define OVE_OBS_GPIO_ODR(port) (0U)
#define OVE_OBS_GPIO_PIN_HIGH(port, n) (0)

static inline uint32_t ove_obs_read32(uintptr_t addr)
{
	(void)addr;
	return 0;
}

#endif /* OVE_RENODE_STM32F746 */

#endif /* OVE_TEST_RENODE_OBS_H */
