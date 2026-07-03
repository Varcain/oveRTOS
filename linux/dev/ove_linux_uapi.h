/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Private uapi mirrors for the Linux-personality device classes: the ioctl
 * command numbers and struct layouts the guest programs pass, spelled with
 * fixed-width types so the binary layout matches the ARM 32-bit kernel (and the
 * host cmocka build). Guest test programs compile against the REAL buildroot
 * linux-headers — that is the layout-drift enforcement.
 */

#ifndef OVE_LINUX_UAPI_H
#define OVE_LINUX_UAPI_H

#include <stdint.h>

/* ---- framebuffer (linux/fb.h) ---------------------------------------------- */
#define OVE_LNX_FBIOGET_VSCREENINFO 0x4600ul
#define OVE_LNX_FBIOPUT_VSCREENINFO 0x4601ul
#define OVE_LNX_FBIOGET_FSCREENINFO 0x4602ul
#define OVE_LNX_FBIOPAN_DISPLAY 0x4606ul
#define OVE_LNX_FBIOBLANK 0x4611ul

/* fb_fix_screeninfo.type / .visual */
#define OVE_LNX_FB_TYPE_PACKED_PIXELS 0
#define OVE_LNX_FB_VISUAL_TRUECOLOR 2

/* One color channel's position within a pixel. */
struct ove_lnx_fb_bitfield {
	uint32_t offset;    /* bit position of the LSB */
	uint32_t length;    /* number of bits */
	uint32_t msb_right; /* != 0 if the MSB is on the right */
};

/* struct fb_var_screeninfo — 160 bytes on ARM32 (all fields u32). */
struct ove_lnx_fb_var_screeninfo {
	uint32_t xres, yres;
	uint32_t xres_virtual, yres_virtual;
	uint32_t xoffset, yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct ove_lnx_fb_bitfield red, green, blue, transp;
	uint32_t nonstd;
	uint32_t activate;
	uint32_t height, width; /* physical size in mm (0 = unknown) */
	uint32_t accel_flags;
	uint32_t pixclock;
	uint32_t left_margin, right_margin, upper_margin, lower_margin;
	uint32_t hsync_len, vsync_len;
	uint32_t sync, vmode, rotate, colorspace;
	uint32_t reserved[4];
};

/* struct fb_fix_screeninfo — 68 bytes on ARM32 (unsigned long = u32). */
struct ove_lnx_fb_fix_screeninfo {
	char id[16];
	uint32_t smem_start; /* physical start of the framebuffer */
	uint32_t smem_len;   /* length of the framebuffer in bytes */
	uint32_t type;
	uint32_t type_aux;
	uint32_t visual;
	uint16_t xpanstep, ypanstep, ywrapstep;
	uint32_t line_length; /* bytes per scanline */
	uint32_t mmio_start;
	uint32_t mmio_len;
	uint32_t accel;
	uint16_t capabilities;
	uint16_t reserved[2];
};

/* FBIOBLANK arg. */
#define OVE_LNX_FB_BLANK_UNBLANK 0

#endif /* OVE_LINUX_UAPI_H */
