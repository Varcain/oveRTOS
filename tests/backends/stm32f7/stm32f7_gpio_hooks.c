/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * test_gpio hooks for targets that link the real STM32F7 GPIO HAL
 * (freertos_gpio.c) instead of stub_gpio.c.
 *
 * The suite asks two things of the backend under test: "is this line's
 * interrupt armed?" and "make hw_unregister fail".  stub_gpio.c answers
 * both from its in-memory tables.  Here the first reads the EXTI interrupt
 * mask the real HAL drives, and the second interposes on the HAL entry
 * point — link with --wrap=ove_hal_gpio_irq_hw_unregister.
 */

#include "ove/hal/hal_gpio.h"
#include "stm32f7xx_hal.h"

static int gpio_irq_unregister_result = OVE_OK;

extern int __real_ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin);

/* EXTI lines are per pin number: every port's pin N shares line N, so the
 * port does not take part in the lookup. */
int stub_gpio_irq_is_armed(unsigned int port, unsigned int pin)
{
	(void)port;
	return pin < 16U ? (int)((EXTI->IMR >> pin) & 1U) : 0;
}

void stub_gpio_set_irq_unregister_result(int result)
{
	gpio_irq_unregister_result = result;
}

int __wrap_ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	if (gpio_irq_unregister_result != OVE_OK)
		return gpio_irq_unregister_result;
	return __real_ove_hal_gpio_irq_hw_unregister(port, pin);
}
