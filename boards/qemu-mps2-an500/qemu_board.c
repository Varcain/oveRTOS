/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Board initialization for QEMU MPS2-AN500: the emulated machine needs no
 * clock, pin or peripheral setup before the backends start.
 */

#include "ove/hal/hal_board.h"
#include "ove/types.h"

int ove_hal_board_init(void)
{
	return OVE_OK;
}
