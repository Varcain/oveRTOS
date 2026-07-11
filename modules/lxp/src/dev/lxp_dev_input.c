/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * /dev/input/event0 evdev touch class for the Linux personality. A single-touch
 * stream (ABS_X, ABS_Y, BTN_TOUCH, SYN_REPORT) fed by an engine-neutral feeder
 * (lxp_input_report_touch) — from the FT5336 driver on real hardware, or the
 * synthetic testpad injector on QEMU. LVGL's lv_evdev reads the raw events
 * (O_NONBLOCK) and needs no ioctls; a couple of EVIOCG* are provided for evtest.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_DEV_INPUT)

#include "lxp/lxp_dev.h"
#include "ove/time.h"
#include "lxp/lxp_disp_ops.h"
#include "lxp_uapi.h"
#if defined(CONFIG_OVE_FT5336)
#include "ove/ft5336.h"
#endif

#include <string.h>

/* Display geometry for the touch clamps (was board_desc.h OVE_DISPLAY_*). Default
 * is the STM32F746-Disco panel; a host overrides via lxp_disp_set_geometry. */
static int g_disp_w = 480, g_disp_h = 272;

void lxp_disp_set_geometry(int width, int height)
{
	if (width > 0)
		g_disp_w = width;
	if (height > 0)
		g_disp_h = height;
}

/* A shared monotonic event ring; each open() tracks its own tail cursor. */
#define LXP_IN_RING 64
static struct lxp_input_event g_in_ring[LXP_IN_RING];
static uint32_t g_in_head; /* total events ever produced (slot = head % RING) */

static void ring_push(uint16_t type, uint16_t code, int32_t value)
{
	uint64_t us = 0;
	lxp_time_us(&us);
	struct lxp_input_event *e = &g_in_ring[g_in_head % LXP_IN_RING];
	e->sec = (uint32_t)(us / 1000000u);
	e->usec = (uint32_t)(us % 1000000u);
	e->type = type;
	e->code = code;
	e->value = value;
	g_in_head++;
}

/* Engine-neutral feeder: one single-touch report → 4 events + wake a blocked reader.
 * Strong override of the weak stub in the device core. */
void lxp_input_report_touch(int x, int y, int pressed)
{
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= g_disp_w)
		x = g_disp_w - 1;
	if (y >= g_disp_h)
		y = g_disp_h - 1;
	ring_push(LXP_EV_ABS, LXP_ABS_X, x);
	ring_push(LXP_EV_ABS, LXP_ABS_Y, y);
	ring_push(LXP_EV_KEY, LXP_BTN_TOUCH, pressed ? 1 : 0);
	ring_push(LXP_EV_SYN, LXP_SYN_REPORT, 0);
	lxp_dev_kick(); /* resume a parked reader promptly */
}

static long in_read(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p, void *buf,
		    size_t len)
{
	(void)d;
	(void)p;
	uint32_t tail = o->u.input.tail;
	/* Overflow: the reader fell more than a ring behind → drop to the oldest kept
	 * event and report SYN_DROPPED once. */
	if (g_in_head - tail > LXP_IN_RING) {
		tail = g_in_head - LXP_IN_RING;
		o->u.input.overrun = 1;
	}
	if (tail == g_in_head)
		return -LXP_EAGAIN; /* empty → O_NONBLOCK returns EAGAIN, else park */

	size_t nmax = len / sizeof(struct lxp_input_event);
	if (nmax == 0)
		return -LXP_EINVAL;
	size_t avail = g_in_head - tail;
	if (nmax > avail)
		nmax = avail;
	struct lxp_input_event *out = buf;
	for (size_t i = 0; i < nmax; i++)
		out[i] = g_in_ring[(tail + i) % LXP_IN_RING];
	o->u.input.tail = (uint16_t)(tail + nmax);
	return (long)(nmax * sizeof(struct lxp_input_event));
}

static unsigned in_poll(struct lxp_dev *d, struct lxp_dev_open *o)
{
	(void)d;
	return (o->u.input.tail != g_in_head) ? LXP_POLLIN : 0;
}

static long in_open(struct lxp_dev *d, struct lxp_dev_open *o, int flags)
{
	(void)d;
	(void)flags;
	o->u.input.tail = (uint16_t)g_in_head; /* start at the live head — no stale backlog */
	o->u.input.overrun = 0;
	return 0;
}

static long in_ioctl(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		     unsigned long cmd, unsigned long arg)
{
	(void)d;
	(void)o;
	/* LVGL needs none of these; provided for evtest / evread. */
	if (cmd == LXP_EVIOCGVERSION) {
		int *v = (void *)arg;
		if (!user_ok(p, v, sizeof(*v), 1))
			return -LXP_EFAULT;
		*v = 0x010001; /* EV_VERSION */
		return 0;
	}
	if (cmd == LXP_EVIOCGID) {
		struct lxp_input_id *id = (void *)arg;
		if (!user_ok(p, id, sizeof(*id), 1))
			return -LXP_EFAULT;
		id->bustype = LXP_BUS_I2C;
		id->vendor = id->product = id->version = 1;
		return 0;
	}
	if (cmd == LXP_EVIOCGRAB)
		return 0; /* single reader — grab is a no-op */
	/* EVIOCGABS(ABS_X/ABS_Y): report the panel extent so a calibrated reader scales. */
	if (LXP_EVIOC_TYPE(cmd) == LXP_EVIOC_E) {
		unsigned nr = LXP_EVIOC_NR(cmd);
		if (nr == LXP_EVIOCGABS_BASE + LXP_ABS_X ||
		    nr == LXP_EVIOCGABS_BASE + LXP_ABS_Y) {
			struct lxp_input_absinfo *a = (void *)arg;
			if (!user_ok(p, a, sizeof(*a), 1))
				return -LXP_EFAULT;
			memset(a, 0, sizeof(*a));
			a->maximum = (nr == LXP_EVIOCGABS_BASE + LXP_ABS_X)
					     ? g_disp_w - 1
					     : g_disp_h - 1;
			return 0;
		}
		if (nr == LXP_EVIOCGNAME_NR) {
			char *nm = (void *)arg;
			static const char name[] = "overtos-touch";
			if (!user_ok(p, nm, sizeof(name), 1))
				return -LXP_EFAULT;
			memcpy(nm, name, sizeof(name));
			return (long)sizeof(name);
		}
	}
	return -LXP_ENOTTY;
}

static const struct lxp_dev_ops in_ops = {
	.open = in_open,
	.read = in_read,
	.ioctl = in_ioctl,
	.poll = in_poll,
};

/* ---- QEMU synthetic testpad: replay a canned gesture from the tick ---------- */
#if defined(CONFIG_OVE_LINUX_DEV_INPUT_TESTPAD)
static void testpad_tick(uint64_t now_us)
{
	/* A slow diagonal drag across the panel, then release, looping — deterministic
	 * input with no host channel (proves the evdev path + drives LVGL's pointer). */
	static uint64_t t0, last_us;
	if (t0 == 0)
		t0 = now_us;
	if (now_us - last_us < 100000u)
		return; /* one report per 100 ms step */
	last_us = now_us;
	uint64_t phase = ((now_us - t0) / 100000u) % 12u; /* 100 ms steps, 1.2 s cycle */
	int x = (int)(phase * g_disp_w / 12u);
	int y = (int)(phase * g_disp_h / 12u);
	lxp_input_report_touch(x, y, phase != 11);
}
#endif

#if defined(CONFIG_OVE_FT5336)
/* Poll the FT5336 controller over i2c (~60 Hz) and report the primary touch. */
static void ft5336_tick(uint64_t now_us)
{
	static uint64_t last_us;
	if (now_us - last_us < 16000u)
		return;
	last_us = now_us;
	int x, y, pressed;
	if (g_lxp_disp_ops->touch_read(&x, &y, &pressed) == 0)
		lxp_input_report_touch(x, y, pressed);
}
#endif

void lxp_dev_autoreg_input(void)
{
	struct lxp_dev dev = {
		.path = "/dev/input/event0",
		.ops = &in_ops,
		.major = 13,
		.minor = 64,
		.size = 0, /* not seekable */
	};
	if (lxp_dev_register(&dev) != 0)
		return;
	/* Prefer a real touch panel (FT5336) when present; the synthetic testpad is the
	 * fallback for QEMU (no touch HW) or if the FT5336 does not probe. Registering
	 * both would let two sources drive one /dev/input/event0 — garbage. */
	int touch_ready = 0;
	(void)touch_ready;
#if defined(CONFIG_OVE_FT5336)
	if (g_lxp_disp_ops->touch_init() == 0) {
		lxp_dev_tick_register(ft5336_tick); /* real HW touch panel */
		touch_ready = 1;
	}
#endif
#if defined(CONFIG_OVE_LINUX_DEV_INPUT_TESTPAD)
	if (!touch_ready)
		lxp_dev_tick_register(testpad_tick); /* QEMU synthetic gestures */
#endif
}

#endif /* CONFIG_OVE_LINUX_DEV_INPUT */
