/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * oveRTOS host adapter for the Linux personality's display / input port
 * (struct ove_lnx_disp_ops). It bridges the /dev/fb0 + /dev/input class drivers
 * to the ove_fb framebuffer HAL and the ove_ft5336 touch controller. The fb ops
 * are filled when /dev/fb0 is built; the touch ops when an FT5336 is present
 * (otherwise NULL — the input driver falls back to the synthetic testpad).
 *
 * g_ove_lnx_disp_ops is statically pointed here, so linking this TU wires the
 * personality to the display HAL with no init call.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_DEV)

#include "ove/linux/disp_ops.h"

#if defined(CONFIG_OVE_LINUX_DEV_FB)
#include "ove/fb.h"
static int d_fb_init(void)
{
	return ove_fb_init();
}
static int d_fb_get_info(struct ove_fb_info *info)
{
	return ove_fb_get_info(info);
}
static void *d_fb_get_buffer(void)
{
	return ove_fb_get_buffer();
}
static void d_fb_flush(int x, int y, int w, int h)
{
	ove_fb_flush(x, y, w, h);
}
static void d_fb_present(void)
{
	ove_fb_present();
}
#endif /* CONFIG_OVE_LINUX_DEV_FB */

#if defined(CONFIG_OVE_FT5336)
#include "ove/ft5336.h"
static int d_touch_init(void)
{
	return ove_ft5336_init();
}
static int d_touch_read(int *x, int *y, int *pressed)
{
	return ove_ft5336_read(x, y, pressed);
}
#endif /* CONFIG_OVE_FT5336 */

static const struct ove_lnx_disp_ops g_ove_adapter_disp_ops = {
#if defined(CONFIG_OVE_LINUX_DEV_FB)
	.fb_init = d_fb_init,
	.fb_get_info = d_fb_get_info,
	.fb_get_buffer = d_fb_get_buffer,
	.fb_flush = d_fb_flush,
	.fb_present = d_fb_present,
#endif
#if defined(CONFIG_OVE_FT5336)
	.touch_init = d_touch_init,
	.touch_read = d_touch_read,
#endif
};

const struct ove_lnx_disp_ops *g_ove_lnx_disp_ops = &g_ove_adapter_disp_ops;

#endif /* CONFIG_OVE_LINUX_DEV */
