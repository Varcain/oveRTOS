/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file ove/lvgl.h
 * @brief Unified LVGL header — single include for application code.
 *
 * @defgroup ove_lvgl LVGL Display Integration
 * @brief Unified LVGL include that combines the oveRTOS abstraction API
 *        and the upstream LVGL library headers.
 *
 * Including this file gives access to:
 * - The oveRTOS LVGL API (init, lock, unlock, tick, handler) via
 *   @c ove/lvgl_internal.h.
 * - All upstream LVGL types and functions.  On Zephyr, @c lvgl.h is placed
 *   directly on the include path; on other backends the @c lvgl/ subdirectory
 *   is used.
 *
 * Usage (C):
 * @code
 *   #include "ove/lvgl.h"
 * @endcode
 *
 * Usage (C++):
 * @code
 *   #include <ove/lvgl.hpp>   // includes this file automatically
 * @endcode
 *
 * @note Requires @c CONFIG_OVE_LVGL for the upstream LVGL headers to be
 *       included.  The oveRTOS abstraction stubs are always present.
 * @{
 */

/*
 * Unified LVGL header — single include for application code.
 *
 * Provides both the oveRTOS LVGL API (init, lock, unlock, tick, handler)
 * and the upstream LVGL types/functions.  Zephyr places lvgl.h directly
 * on the include path; other backends use the lvgl/ subdirectory.
 *
 * Usage (C):
 *   #include "ove/lvgl.h"
 *
 * Usage (C++):
 *   #include <ove/lvgl.hpp>   // includes this automatically
 */

#ifndef OVE_LVGL_H
#define OVE_LVGL_H

/* oveRTOS LVGL abstraction API */
#include "ove/lvgl_internal.h"

/* Upstream LVGL — only include when LVGL is enabled */
#ifdef CONFIG_OVE_LVGL
#if defined(__ZEPHYR__)
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif
#endif /* CONFIG_OVE_LVGL */

/** @} */

#endif /* OVE_LVGL_H */
