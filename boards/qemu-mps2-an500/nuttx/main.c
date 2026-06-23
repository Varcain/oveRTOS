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

#ifndef CONFIG_OVE_LINUX
extern int ove_sim_board_init(void);
#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

#ifndef CONFIG_OVE_LINUX
	ove_sim_board_init(); /* sim display/audio transport; personality is headless */
#endif
	ove_app_run();
	return 0;
}
