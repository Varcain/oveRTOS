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
#define LXP_FBIOGET_VSCREENINFO 0x4600ul
#define LXP_FBIOPUT_VSCREENINFO 0x4601ul
#define LXP_FBIOGET_FSCREENINFO 0x4602ul
#define LXP_FBIOPAN_DISPLAY 0x4606ul
#define LXP_FBIOBLANK 0x4611ul

/* fb_fix_screeninfo.type / .visual */
#define LXP_FB_TYPE_PACKED_PIXELS 0
#define LXP_FB_VISUAL_TRUECOLOR 2

/* One color channel's position within a pixel. */
struct lxp_fb_bitfield {
	uint32_t offset;    /* bit position of the LSB */
	uint32_t length;    /* number of bits */
	uint32_t msb_right; /* != 0 if the MSB is on the right */
};

/* struct fb_var_screeninfo — 160 bytes on ARM32 (all fields u32). */
struct lxp_fb_var_screeninfo {
	uint32_t xres, yres;
	uint32_t xres_virtual, yres_virtual;
	uint32_t xoffset, yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct lxp_fb_bitfield red, green, blue, transp;
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
struct lxp_fb_fix_screeninfo {
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
#define LXP_FB_BLANK_UNBLANK 0

/* ---- input / evdev (linux/input.h) ----------------------------------------- */
/* struct input_event on ARM32: the kernel uapi uses __kernel_ulong_t (32-bit)
 * for the timestamp even with a 64-bit-time_t libc, so this stays 16 bytes. */
struct lxp_input_event {
	uint32_t sec;
	uint32_t usec;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

#define LXP_EV_SYN 0x00
#define LXP_EV_KEY 0x01
#define LXP_EV_ABS 0x03
#define LXP_SYN_REPORT 0
#define LXP_SYN_DROPPED 3
#define LXP_BTN_TOUCH 0x14a
#define LXP_ABS_X 0x00
#define LXP_ABS_Y 0x01

/* struct input_id (EVIOCGID). */
struct lxp_input_id {
	uint16_t bustype, vendor, product, version;
};
#define LXP_BUS_I2C 0x18

/* struct input_absinfo (EVIOCGABS). */
struct lxp_input_absinfo {
	int32_t value, minimum, maximum, fuzz, flat, resolution;
};

/* evdev ioctls (fixed-arg ones; the size-encoded EVIOCGNAME/GBIT are matched by
 * their 'E'-type + nr, ignoring the encoded length). */
#define LXP_EVIOCGVERSION 0x80044501ul     /* _IOR('E',0x01,int) */
#define LXP_EVIOCGID 0x80084502ul	       /* _IOR('E',0x02,input_id) */
#define LXP_EVIOCGRAB 0x40044590ul	       /* _IOW('E',0x90,int) */
#define LXP_EVIOC_TYPE(cmd) (((cmd) >> 8) & 0xff) /* 'E' == 0x45 for evdev */
#define LXP_EVIOC_NR(cmd) ((cmd) & 0xff)
#define LXP_EVIOC_E 0x45
#define LXP_EVIOCGNAME_NR 0x06
#define LXP_EVIOCGABS_BASE 0x40 /* EVIOCGABS(abs) nr = 0x40 + abs */

#endif /* OVE_LINUX_UAPI_H */
