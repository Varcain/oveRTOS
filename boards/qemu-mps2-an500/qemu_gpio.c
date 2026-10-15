/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * GPIO for QEMU MPS2-AN500, shared by every engine. QEMU does not emulate the
 * AN500 GPIO blocks, so the ports board.yaml declares are a RAM bank: a pin
 * reads back the last value written. Nothing drives an input, so a
 * registered pin interrupt never fires.
 */

#include "ove/hal/hal_gpio.h"
#include "board_desc.h"

static unsigned char gpio_state[OVE_GPIO_PORT_COUNT][OVE_GPIO_PINS_PER_PORT];

static int gpio_valid(unsigned int port, unsigned int pin)
{
	return port < OVE_GPIO_PORT_COUNT && pin < OVE_GPIO_PINS_PER_PORT;
}

int ove_hal_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
{
	(void)mode;
	return gpio_valid(port, pin) ? OVE_OK : OVE_ERR_INVALID_PARAM;
}

int ove_hal_gpio_set(unsigned int port, unsigned int pin, int value)
{
	if (!gpio_valid(port, pin)) {
		return OVE_ERR_INVALID_PARAM;
	}
	gpio_state[port][pin] = value ? 1 : 0;
	return OVE_OK;
}

int ove_hal_gpio_get(unsigned int port, unsigned int pin)
{
	if (!gpio_valid(port, pin)) {
		return OVE_ERR_INVALID_PARAM;
	}
	return gpio_state[port][pin];
}

int ove_hal_gpio_irq_hw_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode)
{
	(void)mode;
	return gpio_valid(port, pin) ? OVE_OK : OVE_ERR_INVALID_PARAM;
}

int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin)
{
	return gpio_valid(port, pin) ? OVE_OK : OVE_ERR_INVALID_PARAM;
}

int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin)
{
	return gpio_valid(port, pin) ? OVE_OK : OVE_ERR_INVALID_PARAM;
}

int ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	return ove_hal_gpio_irq_hw_disable(port, pin);
}
