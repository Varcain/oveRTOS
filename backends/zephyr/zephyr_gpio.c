/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/hal/hal_gpio.h"
#include "ove_backend_common.h"
#include <stdatomic.h>
#include <zephyr/drivers/gpio.h>

#define GPIO_IRQ_MAX 8
#define GPIO_IRQ_FREE 0
#define GPIO_IRQ_RESERVING 1
#define GPIO_IRQ_REGISTERED 2

struct zephyr_irq_entry {
	const struct device *dev;
	gpio_pin_t pin;
	gpio_flags_t irq_flags;
	struct gpio_callback cb_data;
	unsigned int port;
	atomic_int registered;
};

static struct zephyr_irq_entry zephyr_irq_table[GPIO_IRQ_MAX];

static const struct device *port_to_dev(unsigned int port)
{
	switch (port) {
	case 0:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioa));
	case 1:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpiob));
	case 2:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioc));
	case 3:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpiod));
	case 4:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioe));
	case 5:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpiof));
	case 6:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpiog));
	case 7:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioh));
	case 8:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpioi));
	default:
		return NULL;
	}
}

extern void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin);

static void zephyr_gpio_irq_handler(const struct device *dev, struct gpio_callback *cb,
				    uint32_t pins)
{
	struct zephyr_irq_entry *entry = CONTAINER_OF(cb, struct zephyr_irq_entry, cb_data);

	if (entry->dev == dev && (pins & BIT(entry->pin)))
		ove_gpio_irq_dispatch(entry->port, (unsigned int)entry->pin);
}

int ove_hal_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
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

int ove_hal_gpio_irq_hw_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode)
{
	const struct device *dev;
	gpio_flags_t flags;
	unsigned int i;
	int ret;

	dev = port_to_dev(port);
	if (dev == NULL || !device_is_ready(dev)) {
		return OVE_ERR_INVALID_PARAM;
	}

	flags = 0;
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
	default:
		return OVE_ERR_INVALID_PARAM;
	}

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		int expected = GPIO_IRQ_FREE;
		if (atomic_compare_exchange_strong_explicit(
			    &zephyr_irq_table[i].registered, &expected, GPIO_IRQ_RESERVING,
			    memory_order_acq_rel, memory_order_acquire)) {
			break;
		}
	}
	if (i >= GPIO_IRQ_MAX) {
		return OVE_ERR_NO_MEMORY;
	}

	ret = gpio_pin_configure(dev, pin, GPIO_INPUT);
	if (ret != 0) {
		atomic_store_explicit(&zephyr_irq_table[i].registered, GPIO_IRQ_FREE,
				      memory_order_release);
		return OVE_ERR_NOT_SUPPORTED;
	}

	ret = gpio_pin_interrupt_configure(dev, pin, GPIO_INT_DISABLE);
	if (ret != 0) {
		atomic_store_explicit(&zephyr_irq_table[i].registered, GPIO_IRQ_FREE,
				      memory_order_release);
		return OVE_ERR_NOT_SUPPORTED;
	}

	zephyr_irq_table[i].dev = dev;
	zephyr_irq_table[i].pin = (gpio_pin_t)pin;
	zephyr_irq_table[i].port = port;
	zephyr_irq_table[i].irq_flags = flags;
	gpio_init_callback(&zephyr_irq_table[i].cb_data, zephyr_gpio_irq_handler, BIT(pin));
	ret = gpio_add_callback(dev, &zephyr_irq_table[i].cb_data);
	if (ret != 0) {
		(void)gpio_pin_interrupt_configure(dev, pin, GPIO_INT_DISABLE);
		atomic_store_explicit(&zephyr_irq_table[i].registered, GPIO_IRQ_FREE,
				      memory_order_release);
		return OVE_ERR_NOT_SUPPORTED;
	}
	atomic_store_explicit(&zephyr_irq_table[i].registered, GPIO_IRQ_REGISTERED,
			      memory_order_release);

	return OVE_OK;
}

int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (atomic_load_explicit(&zephyr_irq_table[i].registered, memory_order_acquire) ==
			    GPIO_IRQ_REGISTERED &&
		    zephyr_irq_table[i].port == port &&
		    zephyr_irq_table[i].pin == (gpio_pin_t)pin) {
			int ret = gpio_pin_interrupt_configure(zephyr_irq_table[i].dev, pin,
							       zephyr_irq_table[i].irq_flags);
			return ret == 0 ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (atomic_load_explicit(&zephyr_irq_table[i].registered, memory_order_acquire) ==
			    GPIO_IRQ_REGISTERED &&
		    zephyr_irq_table[i].port == port &&
		    zephyr_irq_table[i].pin == (gpio_pin_t)pin) {
			int ret = gpio_pin_interrupt_configure(zephyr_irq_table[i].dev, pin,
							       GPIO_INT_DISABLE);
			return ret == 0 ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (atomic_load_explicit(&zephyr_irq_table[i].registered, memory_order_acquire) ==
			    GPIO_IRQ_REGISTERED &&
		    zephyr_irq_table[i].port == port &&
		    zephyr_irq_table[i].pin == (gpio_pin_t)pin) {
			int ret = gpio_pin_interrupt_configure(zephyr_irq_table[i].dev, pin,
							       GPIO_INT_DISABLE);
			if (ret != 0)
				return OVE_ERR_NOT_SUPPORTED;
			/* Release the driver callback and free the slot, else a
			 * re-register of this pin adds a second callback and the
			 * handler dispatches the user callback twice. */
			ret = gpio_remove_callback(zephyr_irq_table[i].dev,
						   &zephyr_irq_table[i].cb_data);
			if (ret != 0)
				return OVE_ERR_NOT_SUPPORTED;
			atomic_store_explicit(&zephyr_irq_table[i].registered, GPIO_IRQ_FREE,
					      memory_order_release);
			return OVE_OK;
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}
