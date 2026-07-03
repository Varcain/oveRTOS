/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Weak "no display" framebuffer HAL. A board WITH a framebuffer provides strong
 * overrides (an500 qemu_fb.c, F746 fb_port.c on FreeRTOS / board_init.c on NuttX);
 * a board without one (an521, a bare nuttx) links these weak stubs, so ove_fb_init
 * fails and the /dev/fb0 class simply does not register — no link error, no fb RAM.
 *
 * These MUST be a separate translation unit from ove_fb.c (the forwarders): GCC's
 * default -fno-semantic-interposition binds a same-TU call to a defined weak symbol
 * locally, which at -O3 (e.g. the NuttX build) would ignore + garbage-collect a
 * board's strong override. Inter-object calls force the linker to resolve them,
 * preferring the strong definition.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FB)

#include "ove/fb.h"
#include "ove/hal/hal_fb.h"
#include "ove/types.h"

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

__attribute__((weak)) void ove_hal_fb_present(void)
{
}

#endif /* CONFIG_OVE_FB */
