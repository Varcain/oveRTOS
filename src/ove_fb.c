/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Portable framebuffer layer: a thin forwarder over the board HAL that also owns
 * the "needs present" dirty flag (so a per-scanline write burst coalesces into
 * one push per run-loop tick). See ove/fb.h + ove/hal/hal_fb.h.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FB)

#include "ove/fb.h"
#include "ove/hal/hal_fb.h"
#include "ove/types.h"

static int g_fb_dirty;

int ove_fb_init(void)
{
	g_fb_dirty = 0;
	return ove_hal_fb_init();
}

int ove_fb_get_info(struct ove_fb_info *info)
{
	if (!info)
		return OVE_ERR_INVALID_PARAM;
	return ove_hal_fb_get_info(info);
}

void *ove_fb_get_buffer(void)
{
	return ove_hal_fb_buffer();
}

void ove_fb_flush(int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	g_fb_dirty = 1; /* the tick presents it; partial-rect tracking is a later nicety */
}

void ove_fb_present(void)
{
	if (!g_fb_dirty)
		return;
	g_fb_dirty = 0;
	ove_hal_fb_present();
}

/* Weak "no display" HAL: a board WITH a framebuffer (an500 qemu_fb.c, F746
 * fb_port.c) provides strong overrides; a board without one (an521, a bare
 * nuttx) links these and ove_fb_init fails, so the fb class simply does not
 * register /dev/fb0 — no link error, and no framebuffer RAM is reserved (that
 * lives in the board backend). */
__attribute__((weak)) int ove_hal_fb_init(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}
__attribute__((weak)) int ove_hal_fb_get_info(struct ove_fb_info *info)
{
	(void)info;
	return OVE_ERR_NOT_SUPPORTED;
}
__attribute__((weak)) void *ove_hal_fb_buffer(void)
{
	return 0;
}
__attribute__((weak)) void ove_hal_fb_present(void) {}

#endif /* CONFIG_OVE_FB */
