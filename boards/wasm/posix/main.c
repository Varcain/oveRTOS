/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM/Emscripten entry point.
 *
 * With -sPROXY_TO_PTHREAD, main() runs in a dedicated Web Worker
 * pthread, so blocking calls (pthread_join in start_scheduler) work.
 */

#include "ove/ove.h"
#include "ove_config.h"

#ifdef CONFIG_OVE_SIM
extern int ove_sim_board_init(void);
#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

#ifdef CONFIG_OVE_SIM
	ove_sim_board_init();
#endif

	ove_app_run();
	return 0;
}
