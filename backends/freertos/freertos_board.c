/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"
#include "ove/hal/hal_board.h"
#include "ove/hal/hal_gpio.h"
#include "ove_backend_common.h"
#include "bsp.h"
#include "lv_port_disp.h"
#include "board_desc.h"

int ove_hal_board_init(void)
{
	bsp_boardInit();
#ifndef CONFIG_OVE_LINUX
	/* The Linux-personality build is headless and minimal: it needs only bsp_boardInit
	 * (clock + the external SDRAM/FMC its program pool lives in + the MPU region that makes
	 * that SDRAM executable & non-cacheable). Skip the LVGL display + LED/GPIO bring-up, which
	 * would otherwise pull in the display stack the personality never uses. */
	lv_port_disp_hw_init();

	/* Configure LED pin(s) as output */
#if OVE_LED_COUNT > 0
	{
		unsigned int i;
		for (i = 0; i < OVE_LED_COUNT; i++) {
			ove_hal_gpio_configure(ove_board_leds[i].port, ove_board_leds[i].pin,
					       OVE_GPIO_MODE_OUTPUT_PP);
		}
	}
#endif
#endif /* !CONFIG_OVE_LINUX */

	return OVE_OK;
}
