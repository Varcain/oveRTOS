/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/hal/hal_board.h"
#include "ove/types.h"
#include "ove_backend_common.h"
#include <stdio.h>

int ove_hal_board_init(void)
{
	printf("[BOARD] Board initialized (POSIX/SDL2)\n");
	return OVE_OK;
}
