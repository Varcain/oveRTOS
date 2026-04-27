/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* LVGL v9 config — NuttX variant of STM32F746G-Discovery.
 * Defines the NuttX display-driver flag, then includes the shared board file.
 * LV_USE_NUTTX must precede the include so the lv_conf_common.h #ifndef
 * guard honors the override. */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_NUTTX 1

#include <ove/lv_conf_stm32f746g_discovery.h>

#endif /* LV_CONF_H */
