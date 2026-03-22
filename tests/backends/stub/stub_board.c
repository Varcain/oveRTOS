/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stub board HAL for testing.
 */

#include "ove/hal/hal_board.h"
#include "ove/types.h"

extern void stub_gpio_reset(void);

int ove_hal_board_init(void)
{
	stub_gpio_reset();
	return OVE_OK;
}
