/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"
#include "ove/gpio.h"
#include "ove/hal/hal_gpio.h"
#include "board_desc.h"

#define GPIO_IRQ_MAX 8

struct gpio_irq_entry {
	unsigned int port;
	unsigned int pin;
	ove_gpio_irq_mode_t mode;
	ove_gpio_irq_cb callback;
	void *user_data;
	int registered;
	int enabled;
};

static struct gpio_irq_entry irq_table[GPIO_IRQ_MAX];

static int validate_port_pin(unsigned int port, unsigned int pin)
{
	if (port >= OVE_GPIO_PORT_COUNT ||
	    pin >= OVE_GPIO_PINS_PER_PORT) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_gpio_configure(unsigned int port, unsigned int pin,
			   ove_gpio_mode_t mode)
{
	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}
	return ove_hal_gpio_configure(port, pin, mode);
}

int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}
	return ove_hal_gpio_set(port, pin, value);
}

int ove_gpio_get(unsigned int port, unsigned int pin)
{
	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}
	return ove_hal_gpio_get(port, pin);
}

int ove_gpio_irq_register(unsigned int port, unsigned int pin,
			      ove_gpio_irq_mode_t mode,
			      ove_gpio_irq_cb callback,
			      void *user_data)
{
	unsigned int i;

	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}

	/* Find free slot */
	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (!irq_table[i].registered) {
			break;
		}
	}
	if (i >= GPIO_IRQ_MAX) {
		return OVE_ERR_NO_MEMORY;
	}

	irq_table[i].port = port;
	irq_table[i].pin = pin;
	irq_table[i].mode = mode;
	irq_table[i].callback = callback;
	irq_table[i].user_data = user_data;
	irq_table[i].registered = 1;
	irq_table[i].enabled = 0;

	return ove_hal_gpio_irq_hw_enable(port, pin, mode, callback,
					      user_data);
}

int ove_gpio_irq_enable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (irq_table[i].registered &&
		    irq_table[i].port == port &&
		    irq_table[i].pin == pin) {
			irq_table[i].enabled = 1;
			return OVE_OK;
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_gpio_irq_disable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (irq_table[i].registered &&
		    irq_table[i].port == port &&
		    irq_table[i].pin == pin) {
			irq_table[i].enabled = 0;
			return ove_hal_gpio_irq_hw_disable(port, pin);
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

/* Called by backend HAL when a GPIO interrupt fires */
void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (irq_table[i].registered &&
		    irq_table[i].enabled &&
		    irq_table[i].port == port &&
		    irq_table[i].pin == pin) {
			if (irq_table[i].callback) {
				irq_table[i].callback(port, pin,
						      irq_table[i].user_data);
			}
			break;
		}
	}
}
