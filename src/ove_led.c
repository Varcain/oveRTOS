/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"
#include "ove/led.h"
#include "ove/hal/hal_gpio.h"
#include "board_desc.h"

void ove_led_set(unsigned int led, int on)
{
	if (led >= OVE_LED_COUNT) {
		return;
	}

	unsigned int port = ove_board_leds[led].port;
	unsigned int pin  = ove_board_leds[led].pin;
	int active_low    = ove_board_leds[led].active_low;

	ove_hal_gpio_set(port, pin, active_low ? !on : on);
}

void ove_led_toggle(unsigned int led)
{
	if (led >= OVE_LED_COUNT) {
		return;
	}

	unsigned int port = ove_board_leds[led].port;
	unsigned int pin  = ove_board_leds[led].pin;

	/* Read current value and invert */
	int current = ove_hal_gpio_get(port, pin);
	if (current >= 0) {
		ove_hal_gpio_set(port, pin, current ? 0 : 1);
	}
}

unsigned int ove_led_count(void)
{
	return OVE_LED_COUNT;
}
