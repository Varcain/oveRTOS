/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * oveRTOS host adapter for the Linux personality's display / input port
 * (lxp_display_ops_t). It bridges the /dev/fb0 + /dev/input class drivers
 * to the ove_fb framebuffer HAL and the ove_ft5336 touch controller. The fb ops
 * are filled when /dev/fb0 is built; the touch ops when an FT5336 is present
 * (otherwise NULL — the input driver falls back to the synthetic testpad).
 *
 * The exported provider table is passed explicitly to lxp_run(), which publishes
 * it only for the duration of the personality run.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_DEV)

#include "lxp/lxp_disp_ops.h"

#if defined(CONFIG_OVE_LINUX_DEV_FB)
#include "ove/fb.h"
static int d_fb_init(void)
{
	return ove_fb_init();
}
static int d_fb_get_info(lxp_fb_info_t *info)
{
	struct ove_fb_info o;
	int r = ove_fb_get_info(&o);
	if (r == 0 && info) {
		info->width = o.width;
		info->height = o.height;
		info->stride_bytes = o.stride_bytes;
		info->fmt = (uint32_t)o.fmt;
		info->smem_len = o.smem_len;
	}
	return r;
}
static void *d_fb_get_buffer(void)
{
	return ove_fb_get_buffer();
}
static void d_fb_present(int x, int y, int w, int h)
{
	ove_fb_present(x, y, w, h);
}
#endif /* CONFIG_OVE_LINUX_DEV_FB */

#if defined(CONFIG_OVE_LINUX_DEV_DMA2D)
#include "ove/hal/hal_dma2d.h"
static int d_dma2d_init(void)
{
	return ove_hal_dma2d_init();
}
/* Bridge the validated lxp DMA2D op to the board HAL (field copy: the lxp op and
 * ove desc are the same layout, but lxp types must not leak into the ove HAL). */
static int d_dma2d_submit(const lxp_dma2d_op_t *op)
{
	ove_dma2d_desc_t d = {
		.mode = op->mode, .w = op->w, .h = op->h,
		.out_addr = op->out_addr, .out_offset = op->out_offset,
		.out_cf = op->out_cf, .out_color = op->out_color,
		.fg_addr = op->fg_addr, .fg_offset = op->fg_offset, .fg_cf = op->fg_cf,
		.fg_color = op->fg_color, .fg_alpha_mode = op->fg_alpha_mode, .fg_alpha = op->fg_alpha,
		.bg_addr = op->bg_addr, .bg_offset = op->bg_offset, .bg_cf = op->bg_cf,
		.bg_color = op->bg_color, .bg_alpha_mode = op->bg_alpha_mode, .bg_alpha = op->bg_alpha,
	};
	return ove_hal_dma2d_submit(&d);
}
#endif /* CONFIG_OVE_LINUX_DEV_DMA2D */

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
static void d_touch_deinit(void)
{
	ove_ft5336_deinit();
}
#endif /* CONFIG_OVE_FT5336 */

const lxp_display_ops_t g_lxp_host_display_ops = {
	.abi_version = LXP_DISPLAY_OPS_ABI_VERSION,
	.struct_size = sizeof(lxp_display_ops_t),
#if defined(CONFIG_OVE_LINUX_DEV_FB)
	.fb_init = d_fb_init,
	.fb_get_info = d_fb_get_info,
	.fb_get_buffer = d_fb_get_buffer,
	.fb_present = d_fb_present,
#endif
#if defined(CONFIG_OVE_LINUX_DEV_DMA2D)
	.dma2d_init = d_dma2d_init,
	.dma2d_submit = d_dma2d_submit,
#endif
#if defined(CONFIG_OVE_FT5336)
	.touch_init = d_touch_init,
	.touch_read = d_touch_read,
	.touch_deinit = d_touch_deinit,
#endif
};

#endif /* CONFIG_OVE_LINUX_DEV */
