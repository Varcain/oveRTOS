/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LVGL_INTERNAL_H
#define OVE_LVGL_INTERNAL_H

/**
 * @defgroup ove_lvgl_internal LVGL Internal Helpers
 * @brief Internal oveRTOS abstraction layer for LVGL display integration.
 *
 * This header declares the oveRTOS-specific LVGL lifecycle functions.
 * Application code should include @c ove/lvgl.h instead, which provides
 * both this API and the upstream LVGL headers in one include.
 *
 * The backend is responsible for registering display and input drivers
 * with LVGL during ove_lvgl_init(); the application only needs to call
 * ove_lvgl_tick() and ove_lvgl_handler() periodically.
 *
 * @note Requires @c CONFIG_OVE_LVGL.  When the option is disabled every
 *       function is replaced by a no-op stub.
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_LVGL

/**
 * @brief Initialise the LVGL library and register the board's display driver.
 *
 * Calls @c lv_init(), registers the backend display and (if present) the
 * touch input driver, and starts any required background tasks.
 *
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_lvgl_init(void);

/**
 * @brief Acquire the LVGL mutex before calling any LVGL API.
 *
 * Must be paired with ove_lvgl_unlock().  Nesting is not supported.
 */
void ove_lvgl_lock(void);

/**
 * @brief Release the LVGL mutex after calling LVGL APIs.
 *
 * Must be called after every ove_lvgl_lock().
 */
void ove_lvgl_unlock(void);

/**
 * @brief Advance the LVGL internal tick counter.
 *
 * Call this from a periodic timer or task every @p ms milliseconds.
 * LVGL uses this counter for animations, transitions, and input debouncing.
 *
 * @param[in] ms  Number of milliseconds elapsed since the last call.
 */
void ove_lvgl_tick(uint32_t ms);

/**
 * @brief Process pending LVGL tasks (rendering, input, animations).
 *
 * Must be called regularly from the UI task — typically every
 * @c LV_DISP_DEF_REFR_PERIOD milliseconds.  Call ove_lvgl_lock() and
 * ove_lvgl_unlock() around this call when sharing LVGL with other tasks.
 */
void ove_lvgl_handler(void);

#else /* !CONFIG_OVE_LVGL */

static inline int ove_lvgl_init(void) { return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_lvgl_lock(void) { }
static inline void ove_lvgl_unlock(void) { }
static inline void ove_lvgl_tick(uint32_t ms) { (void)ms; }
static inline void ove_lvgl_handler(void) { }

#endif /* CONFIG_OVE_LVGL */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LVGL_INTERNAL_H */
