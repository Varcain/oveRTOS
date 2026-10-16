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

/* The devicetree places memory on Zephyr; board.yaml must agree. */
BUILD_ASSERT(DT_REG_ADDR(DT_CHOSEN(zephyr_flash)) == OVE_MEMORY_FLASH_START &&
		     DT_REG_SIZE(DT_CHOSEN(zephyr_flash)) == OVE_MEMORY_FLASH_KB * 1024u,
	     "board.yaml flash differs from the devicetree zephyr,flash");
#if DT_NODE_EXISTS(DT_NODELABEL(sdram1))
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(sdram1)) == OVE_MEMORY_SDRAM_START &&
		     DT_REG_SIZE(DT_NODELABEL(sdram1)) == OVE_MEMORY_SDRAM_KB * 1024u,
	     "board.yaml SDRAM differs from the devicetree sdram1 node");
#endif

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
