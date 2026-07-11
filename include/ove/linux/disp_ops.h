/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * The display / input port for the Linux personality. The /dev/fb0 and
 * /dev/input/event0 class drivers reach the panel + touch controller ONLY through
 * these ops, so the personality carries no direct dependency on a particular
 * framebuffer / touch HAL. On oveRTOS the ops are filled by
 * backends/common/lxp_ove_disp_adapter.c (bridging to ove_fb_* / ove_ft5336_*).
 * Display geometry, which used to come from the per-board board_desc.h, is now an
 * injected value (lxp_disp_set_geometry). Renamed to lxp_display_ops_t at the
 * module-extraction rename.
 */

#ifndef OVE_LINUX_DISP_OPS_H
#define OVE_LINUX_DISP_OPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ove_fb_info; /* full definition in <ove/fb.h>; used here only by pointer */

/* The display / input port. fb_* are required when /dev/fb0 is built; touch_* may
 * be NULL when there is no touch controller (the input driver then relies on the
 * synthetic testpad or an external feeder). */
struct lxp_disp_ops {
	int (*fb_init)(void);
	int (*fb_get_info)(struct ove_fb_info *info);
	void *(*fb_get_buffer)(void);
	void (*fb_flush)(int x, int y, int w, int h);
	void (*fb_present)(void);
	int (*touch_init)(void);
	int (*touch_read)(int *x, int *y, int *pressed);
};

/* The active display port. Set by the host (on oveRTOS: statically to the ove_fb /
 * ove_ft5336 adapter). */
extern const struct lxp_disp_ops *g_lxp_disp_ops;

/* Set the display geometry used to clamp / report touch coordinates (replaces the
 * board_desc.h OVE_DISPLAY_* constants). Non-positive args are ignored; the
 * default is 480x272 (the STM32F746-Disco panel). A host with a different panel
 * calls this before the run (a future lxp_run() seeds it from lxp_config_t). */
void lxp_disp_set_geometry(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LINUX_DISP_OPS_H */
