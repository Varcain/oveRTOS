/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * POSIX native host entry point.
 *
 * When CONFIG_OVE_SIM is enabled, the sim framework replaces SDL2 for
 * display and audio, and provides a browser-based dashboard instead.
 */

#include "ove/ove.h"
#include "ove_config.h"

#ifdef CONFIG_OVE_SIM

extern int ove_sim_board_init(void);

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	ove_sim_board_init();
	ove_app_run();
	return 0;
}

#else /* !CONFIG_OVE_SIM */

#include <SDL.h>

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	/* Prevent SDL from bypassing the compositor — avoids full-screen
	 * black flash on WSL2/WSLg when the window is created. */
	SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	ove_app_run();
	SDL_Quit();
	return 0;
}

#endif /* CONFIG_OVE_SIM */
