/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stub GPIO HAL for testing — in-memory state arrays.
 */

#include "ove/hal/hal_gpio.h"
#include <string.h>

/* 9 ports = STM32F7's GPIOA..GPIOI.  test_led / test_gpio reference
 * OVE_LED0 (port 8, pin 1 = PI1, the LD1 on STM32F746G-Discovery) so
 * the same board_desc works on stub/QEMU AND on real silicon — see
 * tests/board_desc.h for the rationale. */
#define BSP_MAX_PORTS 9
#define BSP_MAX_PINS 16

static int gpio_state[BSP_MAX_PORTS][BSP_MAX_PINS];
static int gpio_irq_armed[BSP_MAX_PORTS][BSP_MAX_PINS];
static int gpio_irq_unregister_result = OVE_OK;

int stub_gpio_get_state(unsigned int port, unsigned int pin)
{
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return 0;
	}
	return gpio_state[port][pin];
}

void stub_gpio_reset(void)
{
	memset(gpio_state, 0, sizeof(gpio_state));
	memset(gpio_irq_armed, 0, sizeof(gpio_irq_armed));
	gpio_irq_unregister_result = OVE_OK;
}

int stub_gpio_irq_is_armed(unsigned int port, unsigned int pin)
{
	return port < BSP_MAX_PORTS && pin < BSP_MAX_PINS ? gpio_irq_armed[port][pin] : 0;
}

void stub_gpio_set_irq_unregister_result(int result)
{
	gpio_irq_unregister_result = result;
}

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
	gpio_irq_armed[port][pin] = 0;
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin)
{
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return OVE_ERR_INVALID_PARAM;
	}
	gpio_irq_armed[port][pin] = 1;
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin)
{
	if (port >= BSP_MAX_PORTS || pin >= BSP_MAX_PINS) {
		return OVE_ERR_INVALID_PARAM;
	}
	gpio_irq_armed[port][pin] = 0;
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	if (gpio_irq_unregister_result != OVE_OK)
		return gpio_irq_unregister_result;
	/* No per-registration HW state on the stub; same as a disable. */
	return ove_hal_gpio_irq_hw_disable(port, pin);
}
