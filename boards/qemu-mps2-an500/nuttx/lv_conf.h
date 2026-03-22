/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * LVGL configuration for NuttX QEMU MPS2-AN500.
 *
 * This file serves two purposes:
 * 1. During NuttX compilation: NuttX sets CONFIG_LV_CONF_SKIP via Kconfig,
 *    so LVGL uses autoconf.h instead of this file.
 * 2. During Rust bindgen: clang processes LVGL headers independently and
 *    needs this file to know which features/fonts are enabled.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (64 * 1024)

/* Fonts used by the example app */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Widgets used by the example app */
#define LV_USE_LABEL 1
#define LV_USE_BAR 1

/* Layout */
#define LV_USE_FLEX 1
#define LV_USE_OBSERVER 1

/* Theme */
#define LV_USE_THEME_DEFAULT 1

/* NuttX integration */
#define LV_USE_NUTTX 1

/* Logging off for embedded */
#define LV_USE_LOG 0

#endif /* LV_CONF_H */
