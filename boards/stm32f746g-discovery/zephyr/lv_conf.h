/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* LVGL v9 config — Zephyr variant of STM32F746G-Discovery.
 * All real settings live in <ove/lv_conf_stm32f746g_discovery.h> (board-shared)
 * and <ove/lv_conf_common.h> (framework-shared). Zephyr's LVGL module reads
 * this file via ove_zephyr_copy_lv_conf_for_bindgen so Rust bindgen has a
 * consistent view of which widgets exist. Keep this file empty of knobs. */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <ove/lv_conf_stm32f746g_discovery.h>

#endif /* LV_CONF_H */
