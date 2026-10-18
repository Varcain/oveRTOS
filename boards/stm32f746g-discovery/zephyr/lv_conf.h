/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* LVGL v9 config — Zephyr variant of STM32F746G-Discovery.
 * All real settings live in <ove/lv_conf_stm32f746g_discovery.h> (board-shared)
 * and <ove/lv_conf_common.h> (framework-shared). Rust bindgen reads this file
 * (LV_CONF_PATH, LV_CONF_INCLUDE_SIMPLE) so it has a consistent view of which
 * widgets exist. Keep this file empty of knobs. */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <ove/lv_conf_stm32f746g_discovery.h>

#endif /* LV_CONF_H */
