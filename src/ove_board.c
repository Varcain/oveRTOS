/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"
#include "ove/board.h"
#include "ove/hal/hal_board.h"
#include "board_desc.h"

int ove_board_init(void)
{
	return ove_hal_board_init();
}

const char *ove_board_name(void)
{
	return ove_board_descriptor.name;
}

const struct ove_board_desc *ove_board_desc(void)
{
	return &ove_board_descriptor;
}
