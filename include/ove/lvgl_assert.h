/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * LVGL's assert handler (LV_ASSERT_HANDLER in ove/lv_conf_common.h): included by
 * LVGL's own sources, so it carries nothing but the one declaration.
 */

#ifndef OVE_LVGL_ASSERT_H
#define OVE_LVGL_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

/** Log where an LVGL assert fired, then halt. */
void ove_lvgl_assert_failed(const char *file, int line) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* OVE_LVGL_ASSERT_H */
