/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * LVGL configuration for Zephyr QEMU MPS2-AN500.
 *
 * Zephyr's LVGL integration uses Kconfig (LV_CONF_SKIP) at build time.
 * This file exists for Rust bindgen, which processes LVGL headers with
 * its own clang pass. Including the shared fragment keeps the widget set
 * visible to bindgen in sync with the other boards.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* Match the original minimal stub — embedded default font */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#include <ove/lv_conf_common.h>

#endif /* LV_CONF_H */
