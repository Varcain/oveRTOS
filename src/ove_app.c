/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"
#include "ove/ove.h"

int ove_app_run(void)
{
#ifdef CONFIG_OVE_BOARD
	ove_board_init();
#endif

#ifdef CONFIG_OVE_CONSOLE
	ove_console_init();
#endif

	OVE_LOG("ove: starting %s %s on %s\n",
		    CONFIG_OVE_APP_NAME,
		    CONFIG_OVE_APP_VERSION,
		    OVE_RTOS_NAME);

	ove_main();

	return OVE_OK;
}

void ove_run(void)
{
#ifdef CONFIG_OVE_AUDIO
	ove_audio_start();
#endif
	ove_thread_start_scheduler();
}
