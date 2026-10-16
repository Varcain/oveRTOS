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
#include <zephyr/devicetree.h>
#include <zephyr/toolchain.h>

#if DT_HAS_CHOSEN(zephyr_display)
/* The devicetree describes the display on Zephyr; board.yaml must agree. */
BUILD_ASSERT(DT_PROP(DT_CHOSEN(zephyr_display), width) == OVE_DISPLAY_WIDTH &&
		     DT_PROP(DT_CHOSEN(zephyr_display), height) == OVE_DISPLAY_HEIGHT,
	     "board.yaml display size differs from the devicetree zephyr,display");
#endif

int ove_hal_board_init(void)
{
	/* Configure LED pin(s) as output */
#if defined(CONFIG_OVE_GPIO) && OVE_LED_COUNT > 0
	{
		unsigned int i;
		for (i = 0; i < OVE_LED_COUNT; i++) {
			ove_hal_gpio_configure(ove_board_leds[i].port, ove_board_leds[i].pin,
					       OVE_GPIO_MODE_OUTPUT_PP);
		}
	}
#endif

	return OVE_OK;
}
