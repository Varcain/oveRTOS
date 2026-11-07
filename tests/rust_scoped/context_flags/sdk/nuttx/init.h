/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
enum {
	OSINIT_POWERUP,
	OSINIT_BOOT,
	OSINIT_TASKLISTS,
	OSINIT_MEMORY,
	OSINIT_HARDWARE,
	OSINIT_OSREADY,
	OSINIT_IDLELOOP,
	OSINIT_PANIC
};
extern unsigned g_nx_initstate;
