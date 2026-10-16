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
#include "ove/types.h"
#include "ove_backend_common.h"
#include "board_desc.h"

/* Default no-op board init. A board that needs bring-up before ove_main() provides its own
 * ove_hal_board_init and EXCLUDES this BOARD backend from the NuttX link (ove_nuttx_exclude_
 * backends(BOARD)) — e.g. the STM32F746 board_init.c, which memory-maps the QSPI-XIP rootfs
 * window + applies the FMC read-pipe fix. (A weak-override here does not work: NuttX archives the
 * app sources, so a strong override in the app archive is never pulled once this resolves.) */
int ove_hal_board_init(void)
{
	/* Configure the board.yaml LEDs as outputs, as the other engines do. The Linux
	 * personality leaves them alone. */
#if defined(CONFIG_OVE_GPIO) && OVE_LED_COUNT > 0 && !defined(CONFIG_OVE_LINUX)
	for (unsigned int i = 0; i < OVE_LED_COUNT; i++)
		ove_hal_gpio_configure(ove_board_leds[i].port, ove_board_leds[i].pin,
				       OVE_GPIO_MODE_OUTPUT_PP);
#endif
	return OVE_OK;
}
