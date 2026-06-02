/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/hal/hal_gpio.h"
#include "ove_backend_common.h"

int ove_hal_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
{
	(void)port;
	(void)pin;
	(void)mode;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_hal_gpio_set(unsigned int port, unsigned int pin, int value)
{
	(void)port;
	(void)pin;
	(void)value;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_hal_gpio_get(unsigned int port, unsigned int pin)
{
	(void)port;
	(void)pin;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode,
			       ove_gpio_irq_cb callback, void *user_data)
{
	(void)port;
	(void)pin;
	(void)mode;
	(void)callback;
	(void)user_data;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin)
{
	(void)port;
	(void)pin;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	/* No per-registration HW state to release; mirror hw_disable. */
	return ove_hal_gpio_irq_hw_disable(port, pin);
}
