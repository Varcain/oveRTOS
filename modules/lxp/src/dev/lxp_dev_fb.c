/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * /dev/fb0 framebuffer class driver: exposes the ove_fb HAL as a Linux fbdev so
 * a stock LVGL fbdev program (lv_linux_fbdev, LV_LINUX_FBDEV_MMAP=0) can render
 * under the personality — FBIOGET_*SCREENINFO to size the panel, then pwrite()
 * scanlines. Writes copy into the ove_fb buffer with 16-bit stores (the F746
 * SDRAM is Device-typed when a program region doesn't cover it, so a 4-byte
 * store to a 2-byte-aligned pixel would UsageFault); the run-loop tick presents.
 */

#include "lxp/lxp_config.h"

#if defined(LXP_ENABLE_DEV_FB)

#include "lxp/lxp_port.h"
#include "lxp/lxp_dev.h"
#include "lxp/lxp_disp_ops.h"
#include "lxp/lxp_types.h"
#include "lxp_uapi.h"

#include <string.h>

static lxp_fb_info_t g_fbinfo;

/* Copy `len` bytes with 16-bit stores when both ends are halfword-aligned (fb
 * writes always are: RGB565 pixels at even byte offsets), else byte-by-byte. */
static void fb_copy16(uint8_t *dst, const uint8_t *src, size_t len)
{
	if ((((uintptr_t)dst | (uintptr_t)src | len) & 1u) == 0) {
		uint16_t *d = (uint16_t *)dst;
		const uint16_t *s = (const uint16_t *)src;
		for (size_t i = 0; i < len / 2; i++)
			d[i] = s[i];
	} else {
		for (size_t i = 0; i < len; i++)
			dst[i] = src[i];
	}
}

static long fb_read(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p, void *buf,
		    size_t len)
{
	(void)p;
	uint8_t *fb = g_lxp_disp_ops->fb_get_buffer();
	if (!fb || o->pos >= d->size)
		return 0; /* EOF at/after the buffer end */
	size_t n = d->size - o->pos;
	if (n > len)
		n = len;
	fb_copy16(buf, fb + o->pos, n);
	o->pos += (uint32_t)n;
	return (long)n;
}

static long fb_write(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		     const void *buf, size_t len)
{
	(void)p;
	uint8_t *fb = g_lxp_disp_ops->fb_get_buffer();
	if (!fb || o->pos >= d->size)
		return -LXP_EFBIG; /* a write past the framebuffer end */
	size_t n = d->size - o->pos;
	if (n > len)
		n = len;
	fb_copy16(fb + o->pos, buf, n);
	g_lxp_disp_ops->fb_flush(0, (int)(o->pos / g_fbinfo.stride_bytes), g_fbinfo.width,
		     (int)((n + g_fbinfo.stride_bytes - 1) / g_fbinfo.stride_bytes));
	o->pos += (uint32_t)n;
	return (long)n;
}

static void fill_vinfo(struct lxp_fb_var_screeninfo *v)
{
	memset(v, 0, sizeof(*v));
	v->xres = v->xres_virtual = g_fbinfo.width;
	v->yres = v->yres_virtual = g_fbinfo.height;
	v->bits_per_pixel = 16;
	/* RGB565 channel positions. */
	v->red.offset = 11;
	v->red.length = 5;
	v->green.offset = 5;
	v->green.length = 6;
	v->blue.offset = 0;
	v->blue.length = 5;
}

static void fill_finfo(struct lxp_fb_fix_screeninfo *f)
{
	memset(f, 0, sizeof(*f));
	memcpy(f->id, "ovefb", 5);
	f->smem_start = (uint32_t)(uintptr_t)g_lxp_disp_ops->fb_get_buffer();
	f->smem_len = g_fbinfo.smem_len;
	f->type = LXP_FB_TYPE_PACKED_PIXELS;
	f->visual = LXP_FB_VISUAL_TRUECOLOR;
	f->line_length = g_fbinfo.stride_bytes;
}

static long fb_ioctl(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		     unsigned long cmd, unsigned long arg)
{
	(void)d;
	(void)o;
	switch (cmd) {
	case LXP_FBIOGET_VSCREENINFO: {
		struct lxp_fb_var_screeninfo *v = (void *)arg;
		if (!user_ok(p, v, sizeof(*v), 1))
			return -LXP_EFAULT;
		fill_vinfo(v);
		return 0;
	}
	case LXP_FBIOGET_FSCREENINFO: {
		struct lxp_fb_fix_screeninfo *f = (void *)arg;
		if (!user_ok(p, f, sizeof(*f), 1))
			return -LXP_EFAULT;
		fill_finfo(f);
		return 0;
	}
	case LXP_FBIOPUT_VSCREENINFO: {
		/* Accept iff the requested geometry matches ours (busybox fbset / LVGL's
		 * force-refresh do GET-modify-PUT); we run a single fixed mode. */
		struct lxp_fb_var_screeninfo *v = (void *)arg;
		if (!user_ok(p, v, sizeof(*v), 0))
			return -LXP_EFAULT;
		if (v->xres != g_fbinfo.width || v->yres != g_fbinfo.height ||
		    v->bits_per_pixel != 16)
			return -LXP_EINVAL;
		return 0;
	}
	case LXP_FBIOBLANK:
		return 0; /* no power management; always on */
	case LXP_FBIOPAN_DISPLAY:
		return -LXP_EINVAL; /* no hardware panning */
	default:
		return -LXP_ENOTTY;
	}
}

static unsigned fb_poll(struct lxp_dev *d, struct lxp_dev_open *o)
{
	(void)d;
	(void)o;
	return LXP_POLLOUT; /* always writable, never readable-blocking */
}

/* mmap(2) (P3): hand the guest the framebuffer itself, so a stock LVGL fbdev program
 * with LV_LINUX_FBDEV_MMAP=1 writes pixels straight into it — replacing ~272 per-row
 * pwrite syscalls/frame with a userspace memcpy. Normal-NC: the guest's stores reach
 * SDRAM directly for the LTDC's continuous scanout, with no cache maintenance. The
 * run-loop coordinator installs the actual MPU region (eng->map_device). */
static long fb_mmap(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p, size_t len,
		    uint32_t pgoff, uintptr_t *phys, unsigned *attrs)
{
	(void)o;
	(void)p;
	uint8_t *fb = g_lxp_disp_ops->fb_get_buffer();
	if (!fb || pgoff != 0 || len > d->size)
		return -LXP_EINVAL;
	*phys = (uintptr_t)fb;
	*attrs = LXP_MAP_NC;
	return 0;
}

static const struct lxp_dev_ops fb_ops = {
	.read = fb_read,
	.write = fb_write,
	.ioctl = fb_ioctl,
	.poll = fb_poll,
	.mmap = fb_mmap,
};

/* Present the framebuffer to the display at ~30 Hz (coalesces a per-scanline
 * write burst into one push). Runs on the coordinator thread. */
static void fb_tick(uint64_t now_us)
{
	static uint64_t last_us;
	if (now_us - last_us < 33000ull)
		return;
	last_us = now_us;
	g_lxp_disp_ops->fb_present();
}

void lxp_dev_autoreg_fb(void)
{
	if (g_lxp_disp_ops->fb_init() != 0)
		return; /* no display on this board (e.g. an521) → /dev/fb0 absent */
	if (g_lxp_disp_ops->fb_get_info(&g_fbinfo) != 0)
		return;
	struct lxp_dev dev = {
		.path = "/dev/fb0",
		.ops = &fb_ops,
		.major = 29,
		.minor = 0,
		.size = g_fbinfo.smem_len,
	};
	if (lxp_dev_register(&dev) == 0)
		lxp_dev_tick_register(fb_tick);
}

#endif /* LXP_ENABLE_DEV_FB */
