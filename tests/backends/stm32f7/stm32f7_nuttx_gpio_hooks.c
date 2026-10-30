/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * test_gpio hooks for the STM32F746 NuttX test images, which link the
 * board's GPIO HAL (boards/stm32f746g-discovery/nuttx/src/gpio.c) instead
 * of stub_gpio.c; the NuttX twin of stm32f7_gpio_hooks.c.
 *
 * "Is this line's interrupt armed?" reads the EXTI interrupt mask NuttX's
 * EXTI driver programs, and "make hw_unregister fail" interposes on the HAL
 * entry point — link with --wrap=ove_hal_gpio_irq_hw_unregister.
 */

#include <stdint.h>

#include "ove/hal/hal_gpio.h"

#define STM32F7_EXTI_IMR (*(volatile uint32_t *)0x40013C00u)

static int gpio_irq_unregister_result = OVE_OK;

extern int __real_ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin);

/* EXTI lines are per pin number: every port's pin N shares line N, so the
 * port does not take part in the lookup. */
int stub_gpio_irq_is_armed(unsigned int port, unsigned int pin)
{
	(void)port;
	return pin < 16U ? (int)((STM32F7_EXTI_IMR >> pin) & 1U) : 0;
}

void stub_gpio_set_irq_unregister_result(int result)
{
	gpio_irq_unregister_result = result;
}

/* Called by stub_board.c's board init; the hardware needs no resetting. */
void stub_gpio_reset(void)
{
	gpio_irq_unregister_result = OVE_OK;
}

int __wrap_ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	if (gpio_irq_unregister_result != OVE_OK)
		return gpio_irq_unregister_result;
	return __real_ove_hal_gpio_irq_hw_unregister(port, pin);
}
