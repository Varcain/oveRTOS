/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * POSIX/SDL2 native host entry point.
 */

#include "ove/ove.h"
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
