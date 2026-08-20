/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Portable framebuffer layer: a thin forwarder over the board HAL. Its caller
 * owns update coalescing; this layer owns only the physical framebuffer API.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FB)

#include "ove/fb.h"
#include "ove/hal/hal_fb.h"
#include "ove/types.h"

int ove_fb_init(void)
{
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

void ove_fb_present(int x, int y, int w, int h)
{
	ove_hal_fb_present(x, y, w, h);
}

/* The weak "no display" ove_hal_fb_* stubs live in a SEPARATE object (ove_fb_stub.c)
 * on purpose: GCC's default -fno-semantic-interposition binds a same-TU call to a
 * defined weak symbol locally, so if the stubs were here the forwarders above would
 * call them directly and a board's strong override (an500 qemu_fb.c, F746 fb_port.c /
 * nuttx board_init.c) would be silently ignored + garbage-collected. Keeping the calls
 * inter-object forces the linker to resolve them — strong preferred over weak. */

#endif /* CONFIG_OVE_FB */
