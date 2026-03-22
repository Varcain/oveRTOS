/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Minimal lv_conf.h for Zephyr QEMU MPS2-AN500 Rust bindgen.
 *
 * Zephyr's LVGL integration uses Kconfig (LV_CONF_SKIP) at build time.
 * This file exists only for Rust bindgen which processes LVGL headers
 * independently with its own clang.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_LABEL 1
#define LV_USE_BAR 1
#define LV_USE_FLEX 1
#define LV_USE_OBSERVER 1
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_LOG 0

#endif /* LV_CONF_H */
