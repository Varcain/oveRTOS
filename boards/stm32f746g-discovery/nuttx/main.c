/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "board_desc.h"

#include <arch/board/board.h>

_Static_assert(STM32_HSE_FREQUENCY == OVE_CLOCK_HSE_FREQ_MHZ * 1000000ul,
	       "board.yaml HSE differs from NuttX's board.h");
_Static_assert(STM32_SYSCLK_FREQUENCY == OVE_CLOCK_SYSCLK_MHZ * 1000000ul,
	       "board.yaml sysclk differs from NuttX's board.h");

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	ove_app_run();
	return 0;
}
