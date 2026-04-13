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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_LVGL

/**
 * @brief Keypad read callback — the user implements this to deliver
 *        key events to LVGL via ove_lvgl_register_keypad().
 *
 * @param[out] key      Filled with the current LV_KEY_* code if a key
 *                      is pressed.
 * @param[out] pressed  Filled with @c true if currently pressed,
 *                      @c false if released.
 * @return @c true if the callback wrote `key` and `pressed` (LVGL
 *         should use them), @c false to indicate no new input.
 */
typedef bool (*ove_lvgl_keypad_read_fn_t)(uint32_t *key, bool *pressed);

/**
 * @brief Encoder read callback — the user implements this to deliver
 *        rotation/click events to LVGL.
 *
 * @param[out] diff     Filled with the accumulated encoder delta since
 *                      the last read (positive = clockwise).
 * @param[out] pressed  Filled with @c true if the encoder switch is
 *                      currently pressed.
 * @return @c true if the callback wrote `diff` and `pressed`.
 */
typedef bool (*ove_lvgl_encoder_read_fn_t)(int16_t *diff, bool *pressed);

/**
 * @brief Register a keypad input device with LVGL.
 *
 * On first call the function creates an `lv_indev_t` of type keypad
 * and installs an internal read-callback shim that dispatches to @p cb
 * every refresh cycle. Subsequent calls replace the user callback
 * without creating another indev.
 *
 * Pass @c NULL to deregister (LVGL will still poll but the shim returns
 * "no input").
 *
 * @param[in] cb Callback supplying keypad state, or @c NULL to deregister.
 * @return OVE_OK on success.
 */
int ove_lvgl_register_keypad(ove_lvgl_keypad_read_fn_t cb);

/**
 * @brief Register an encoder input device with LVGL.
 *
 * Same semantics as ove_lvgl_register_keypad() but for rotary encoder
 * (+ push) input.
 */
int ove_lvgl_register_encoder(ove_lvgl_encoder_read_fn_t cb);

/**
 * @brief Returns the keypad LVGL input device handle (`lv_indev_t *`
 *        as @c void*) or @c NULL if none has been registered. Cast to
 *        `lv_indev_t *` at the call site and bind to an `lv_group_t`
 *        via `lv_indev_set_group()`.
 *
 * The return type is `void *` so this header does not need to pull in
 * `<lvgl.h>` or forward-declare LVGL types that conflict with its
 * own typedefs.
 */
void *ove_lvgl_get_keypad_indev(void);

/**
 * @brief Returns the encoder LVGL input device handle as @c void*, or
 *        @c NULL if none has been registered.
 */
void *ove_lvgl_get_encoder_indev(void);


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

typedef bool (*ove_lvgl_keypad_read_fn_t)(uint32_t *key, bool *pressed);
typedef bool (*ove_lvgl_encoder_read_fn_t)(int16_t *diff, bool *pressed);

static inline int ove_lvgl_init(void) { return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_lvgl_lock(void) { }
static inline void ove_lvgl_unlock(void) { }
static inline void ove_lvgl_tick(uint32_t ms) { (void)ms; }
static inline void ove_lvgl_handler(void) { }
static inline int ove_lvgl_register_keypad(ove_lvgl_keypad_read_fn_t cb)
    { (void)cb; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_lvgl_register_encoder(ove_lvgl_encoder_read_fn_t cb)
    { (void)cb; return OVE_ERR_NOT_SUPPORTED; }
static inline void *ove_lvgl_get_keypad_indev(void) { return (void *)0; }
static inline void *ove_lvgl_get_encoder_indev(void) { return (void *)0; }

#endif /* CONFIG_OVE_LVGL */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LVGL_INTERNAL_H */
