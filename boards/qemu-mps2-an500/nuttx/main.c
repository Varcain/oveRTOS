/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU MPS2-AN500 NuttX entry point.
 */

#include "ove/ove.h"

extern int ove_sim_board_init(void);

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	ove_sim_board_init();
	ove_app_run();
	return 0;
}
