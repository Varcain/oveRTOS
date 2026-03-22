/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/hal/hal_gpio.h"
#include "ove_backend_common.h"
#include <zephyr/drivers/gpio.h>

#define GPIO_IRQ_MAX 8

struct zephyr_irq_entry {
	const struct device *dev;
	gpio_pin_t pin;
	struct gpio_callback cb_data;
	unsigned int port;
	int registered;
};

static struct zephyr_irq_entry zephyr_irq_table[GPIO_IRQ_MAX];

static const struct device *port_to_dev(unsigned int port)
{
	switch (port) {
	case 0: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioa));
	case 1: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpiob));
	case 2: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioc));
	case 3: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpiod));
	case 4: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioe));
	case 5: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpiof));
	case 6: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpiog));
	case 7: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioh));
	case 8: return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioi));
	default: return NULL;
	}
}

extern void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin);

static void zephyr_gpio_irq_handler(const struct device *dev,
				    struct gpio_callback *cb,
				    uint32_t pins)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (zephyr_irq_table[i].registered &&
		    zephyr_irq_table[i].dev == dev &&
		    (pins & BIT(zephyr_irq_table[i].pin))) {
			ove_gpio_irq_dispatch(
				zephyr_irq_table[i].port,
				(unsigned int)zephyr_irq_table[i].pin);
		}
	}
}

int ove_hal_gpio_configure(unsigned int port, unsigned int pin,
			       ove_gpio_mode_t mode)
{
	const struct device *dev = port_to_dev(port);
	gpio_flags_t flags;

	if (dev == NULL || !device_is_ready(dev)) {
		return OVE_ERR_INVALID_PARAM;
	}

	switch (mode) {
	case OVE_GPIO_MODE_INPUT:
		flags = GPIO_INPUT;
		break;
	case OVE_GPIO_MODE_OUTPUT_PP:
		flags = GPIO_OUTPUT_INACTIVE;
		break;
	case OVE_GPIO_MODE_OUTPUT_OD:
		flags = GPIO_OUTPUT_INACTIVE | GPIO_OPEN_DRAIN;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}

	int ret = gpio_pin_configure(dev, pin, flags);
	return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

int ove_hal_gpio_set(unsigned int port, unsigned int pin, int value)
{
	const struct device *dev = port_to_dev(port);
	if (dev == NULL || !device_is_ready(dev)) {
		return OVE_ERR_INVALID_PARAM;
	}
	gpio_pin_set(dev, pin, value ? 1 : 0);
	return OVE_OK;
}

int ove_hal_gpio_get(unsigned int port, unsigned int pin)
{
	const struct device *dev = port_to_dev(port);
	if (dev == NULL || !device_is_ready(dev)) {
		return OVE_ERR_INVALID_PARAM;
	}
	int val = gpio_pin_get(dev, pin);
	return (val < 0) ? OVE_ERR_NOT_SUPPORTED : val;
}

int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin,
				   ove_gpio_irq_mode_t mode,
				   ove_gpio_irq_cb callback,
				   void *user_data)
{
	const struct device *dev;
	gpio_flags_t flags;
	unsigned int i;
	int ret;

	(void)callback;
	(void)user_data;

	dev = port_to_dev(port);
	if (dev == NULL || !device_is_ready(dev)) {
		return OVE_ERR_INVALID_PARAM;
	}

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (!zephyr_irq_table[i].registered) {
			break;
		}
	}
	if (i >= GPIO_IRQ_MAX) {
		return OVE_ERR_NO_MEMORY;
	}

	flags = GPIO_INPUT;
	switch (mode) {
	case OVE_GPIO_IRQ_RISING:
		flags |= GPIO_INT_EDGE_RISING;
		break;
	case OVE_GPIO_IRQ_FALLING:
		flags |= GPIO_INT_EDGE_FALLING;
		break;
	case OVE_GPIO_IRQ_BOTH:
		flags |= GPIO_INT_EDGE_BOTH;
		break;
	}

	ret = gpio_pin_configure(dev, pin, flags);
	if (ret != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	ret = gpio_pin_interrupt_configure(dev, pin, flags & ~GPIO_INPUT);
	if (ret != 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	zephyr_irq_table[i].dev = dev;
	zephyr_irq_table[i].pin = (gpio_pin_t)pin;
	zephyr_irq_table[i].port = port;
	zephyr_irq_table[i].registered = 1;

	gpio_init_callback(&zephyr_irq_table[i].cb_data,
			   zephyr_gpio_irq_handler, BIT(pin));
	gpio_add_callback(dev, &zephyr_irq_table[i].cb_data);

	return OVE_OK;
}

int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (zephyr_irq_table[i].registered &&
		    zephyr_irq_table[i].port == port &&
		    zephyr_irq_table[i].pin == (gpio_pin_t)pin) {
			gpio_pin_interrupt_configure(
				zephyr_irq_table[i].dev, pin,
				GPIO_INT_DISABLE);
			return OVE_OK;
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}
