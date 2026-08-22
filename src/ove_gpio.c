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
#include <stdatomic.h>

#define GPIO_IRQ_MAX 8

#define GPIO_IRQ_FREE 0
#define GPIO_IRQ_REGISTERED 1
#define GPIO_IRQ_RESERVING 2

struct gpio_irq_entry {
	unsigned int port;
	unsigned int pin;
	ove_gpio_irq_mode_t mode;
	ove_gpio_irq_cb callback;
	void *user_data;
	atomic_int registered;
	atomic_int enabled;
};

static struct gpio_irq_entry irq_table[GPIO_IRQ_MAX];

static int validate_port_pin(unsigned int port, unsigned int pin)
{
	if (port >= OVE_GPIO_PORT_COUNT || pin >= OVE_GPIO_PINS_PER_PORT) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
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

int ove_gpio_irq_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode,
			  ove_gpio_irq_cb callback, void *user_data)
{
	unsigned int i;

	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}

	/* Reserve a free slot before publishing its callback fields. */
	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		int expected = GPIO_IRQ_FREE;
		if (atomic_compare_exchange_strong_explicit(
			    &irq_table[i].registered, &expected, GPIO_IRQ_RESERVING,
			    memory_order_acq_rel, memory_order_acquire)) {
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
	atomic_store_explicit(&irq_table[i].enabled, 0, memory_order_relaxed);

	int ret = ove_hal_gpio_irq_hw_enable(port, pin, mode, callback, user_data);
	atomic_store_explicit(&irq_table[i].registered,
			      ret == OVE_OK ? GPIO_IRQ_REGISTERED : GPIO_IRQ_FREE,
			      memory_order_release);
	return ret;
}

int ove_gpio_irq_enable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (atomic_load_explicit(&irq_table[i].registered, memory_order_acquire) ==
			    GPIO_IRQ_REGISTERED &&
		    irq_table[i].port == port && irq_table[i].pin == pin) {
			int ret = ove_hal_gpio_irq_hw_enable(port, pin, irq_table[i].mode,
							     irq_table[i].callback,
							     irq_table[i].user_data);
			if (ret != OVE_OK)
				return ret;
			atomic_store_explicit(&irq_table[i].enabled, 1, memory_order_release);
			return OVE_OK;
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_gpio_irq_disable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (atomic_load_explicit(&irq_table[i].registered, memory_order_acquire) ==
			    GPIO_IRQ_REGISTERED &&
		    irq_table[i].port == port && irq_table[i].pin == pin) {
			atomic_store_explicit(&irq_table[i].enabled, 0, memory_order_release);
			return ove_hal_gpio_irq_hw_disable(port, pin);
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_gpio_irq_unregister(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (atomic_load_explicit(&irq_table[i].registered, memory_order_acquire) ==
			    GPIO_IRQ_REGISTERED &&
		    irq_table[i].port == port && irq_table[i].pin == pin) {
			/* Disable the line, then free the slot so the (port,pin)
			 * can be re-registered.  Clearing `enabled` before
			 * `registered` keeps a concurrent dispatch from firing a
			 * half-torn-down entry (it gates on `enabled`). */
			atomic_store_explicit(&irq_table[i].enabled, 0, memory_order_release);
			/* Permanent teardown — hw_unregister (not hw_disable) so
			 * the backend also releases per-registration HW state
			 * (e.g. Zephyr's gpio_callback), otherwise re-registering
			 * the same (port,pin) double-registers and double-fires. */
			int ret = ove_hal_gpio_irq_hw_unregister(port, pin);
			atomic_store_explicit(&irq_table[i].registered, GPIO_IRQ_FREE,
					      memory_order_release);
			return ret;
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

/* Called by backend HAL when a GPIO interrupt fires */
void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (atomic_load_explicit(&irq_table[i].registered, memory_order_acquire) !=
		    GPIO_IRQ_REGISTERED)
			continue;
		if (atomic_load_explicit(&irq_table[i].enabled, memory_order_acquire) &&
		    irq_table[i].port == port && irq_table[i].pin == pin) {
			if (irq_table[i].callback) {
				irq_table[i].callback(port, pin, irq_table[i].user_data);
			}
			break;
		}
	}
}
