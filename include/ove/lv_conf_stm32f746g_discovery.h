/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Board-level LVGL v9 configuration for STM32F746G-Discovery.
 *
 * One source of truth for the three RTOS variants (FreeRTOS, NuttX, Zephyr).
 * Each per-RTOS lv_conf.h is a thin wrapper that defines any RTOS-specific
 * driver flags (currently just LV_USE_NUTTX) and includes this file via
 * <ove/lv_conf_stm32f746g_discovery.h>. The angle-bracket form is required
 * because NuttX/Zephyr stage lv_conf.h into a build directory away from
 * the source tree, so a relative include would not resolve.
 *
 * The cross-target widget/feature/font enables come from
 * <ove/lv_conf_common.h>, included at the bottom.
 *
 * Do not include this file directly outside an lv_conf.h.
 */

#ifndef OVE_LV_CONF_BOARD_STM32F746G_DISCOVERY_H
#define OVE_LV_CONF_BOARD_STM32F746G_DISCOVERY_H

#include <stdint.h>
#include <stddef.h>

/*====================
 * COLOR + MEMORY
 *====================*/

/* RGB565 color display */
#define LV_COLOR_DEPTH 16

/* LVGL memory pool size (32KB for color display) */
#define LV_MEM_SIZE (32 * 1024)

/*=================
 * OPERATING SYSTEM
 *=================*/

/* LV_OS_NONE — display is only accessed from a single task. STM32Cube
 * FreeRTOS doesn't expose atomic.h that LVGL's os wrapper expects, and
 * Zephyr/NuttX use a single LVGL handler thread guarded by oveRTOS locks. */
#define LV_USE_OS   LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/

#define LV_DRAW_BUF_ALIGN           32
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE    (4 * 1024)
#define LV_DRAW_THREAD_STACK_SIZE    (4 * 1024)

#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1
    /* RGB565 only for color LCD */
    #define LV_DRAW_SW_SUPPORT_RGB565       1
    #define LV_DRAW_SW_SUPPORT_RGB565A8     0
    #define LV_DRAW_SW_SUPPORT_RGB888       0
    #define LV_DRAW_SW_SUPPORT_XRGB8888     0
    #define LV_DRAW_SW_SUPPORT_ARGB8888     0
    #define LV_DRAW_SW_SUPPORT_L8           0
    #define LV_DRAW_SW_SUPPORT_AL88         0
    #define LV_DRAW_SW_SUPPORT_A8           0
    #define LV_DRAW_SW_SUPPORT_I1           0

    #define LV_DRAW_SW_DRAW_UNIT_CNT    1
    #define LV_USE_DRAW_ARM2D_SYNC      0
    #define LV_USE_NATIVE_HELIUM_ASM    0
    #define LV_DRAW_SW_COMPLEX          1
    #define LV_USE_DRAW_SW_ASM     LV_DRAW_SW_ASM_NONE
    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS    0
#endif

/*==================
 * DEFAULT FONT
 *==================*/

#define LV_FONT_DEFAULT &lv_font_montserrat_32

/*==================
 * Shared widgets / layouts / themes / features
 *==================*/

#include <ove/lv_conf_common.h>

#endif /* OVE_LV_CONF_BOARD_STM32F746G_DISCOVERY_H */
