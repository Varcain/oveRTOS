/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/hal/hal_gpio.h"
#include "ove_backend_common.h"
#include <stdio.h>
#include <string.h>

#define BSP_MAX_PORTS 8
#define BSP_MAX_PINS 16

static int gpio_state[BSP_MAX_PORTS][BSP_MAX_PINS];

int ove_hal_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
{
	(void)mode;
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_hal_gpio_set(unsigned int port, unsigned int pin, int value)
{
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return OVE_ERR_INVALID_PARAM;
	}
	gpio_state[port][pin] = value ? 1 : 0;
	return OVE_OK;
}

int ove_hal_gpio_get(unsigned int port, unsigned int pin)
{
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return OVE_ERR_INVALID_PARAM;
	}
	return gpio_state[port][pin];
}

int ove_hal_gpio_irq_hw_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode)
{
	(void)mode;
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin)
{
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin)
{
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	/* No per-registration HW state on the host sim; same as a disable. */
	return ove_hal_gpio_irq_hw_disable(port, pin);
}
