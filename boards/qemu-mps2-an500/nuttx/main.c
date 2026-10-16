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
#include "board_desc.h"

#include <arch/board/board.h>

/* Kernel RAM is the board's on-chip RAM (board patches 0001 and 0003). */
_Static_assert(CONFIG_RAM_START == OVE_MEMORY_RAM_START &&
		       CONFIG_RAM_SIZE == OVE_MEMORY_RAM_KB * 1024,
	       "board.yaml RAM differs from NuttX's CONFIG_RAM_START/SIZE");

/* SysTick runs from the system clock (board patch 0002). */
_Static_assert(MPS_SYSTICK_CLOCK == OVE_CLOCK_SYSCLK_MHZ * 1000 * 1000,
	       "board.yaml sysclk differs from NuttX's MPS_SYSTICK_CLOCK");

#ifndef CONFIG_OVE_LINUX
extern int ove_sim_board_init(void);
#endif
#ifdef CONFIG_OVE_NET
extern int qemu_netdev_init(void);
#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

#ifndef CONFIG_OVE_LINUX
	ove_sim_board_init(); /* sim display/audio transport; personality is headless */
#endif
#ifdef CONFIG_OVE_NET
	(void)qemu_netdev_init(); /* eth0 must exist before the app configures it */
#endif
	ove_app_run();
	return 0;
}
