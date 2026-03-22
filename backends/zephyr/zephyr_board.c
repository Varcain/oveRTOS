/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/hal/hal_board.h"
#include "ove/hal/hal_gpio.h"
#include "ove_backend_common.h"
#include "board_desc.h"

int ove_hal_board_init(void)
{
	/* Configure LED pin(s) as output */
#if OVE_LED_COUNT > 0
	{
		unsigned int i;
		for (i = 0; i < OVE_LED_COUNT; i++) {
			ove_hal_gpio_configure(
				ove_board_leds[i].port,
				ove_board_leds[i].pin,
				OVE_GPIO_MODE_OUTPUT_PP);
		}
	}
#endif

	return OVE_OK;
}
