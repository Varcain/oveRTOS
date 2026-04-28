/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * LVGL v9 configuration for QEMU MPS2-AN500 FreeRTOS.
 *
 * Board-specific knobs live here; shared widgets/features come from
 * <ove/lv_conf_common.h>.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include <stddef.h>

/*====================
 * COLOR + MEMORY
 *====================*/

/* RGB565 color display */
#define LV_COLOR_DEPTH 16

/* Generous on QEMU (4MB RAM) */
#define LV_MEM_SIZE (64 * 1024)

/*=================
 * OPERATING SYSTEM
 *=================*/

/* Single-task access with external mutex */
#define LV_USE_OS LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/

#define LV_DRAW_BUF_ALIGN 4
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (4 * 1024)
#define LV_DRAW_THREAD_STACK_SIZE (4 * 1024)

#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB565A8 0
#define LV_DRAW_SW_SUPPORT_RGB888 1
#define LV_DRAW_SW_SUPPORT_XRGB8888 1
#define LV_DRAW_SW_SUPPORT_ARGB8888 1
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_A8 0
#define LV_DRAW_SW_SUPPORT_I1 0

#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_DRAW_ARM2D_SYNC 0
#define LV_USE_NATIVE_HELIUM_ASM 0
#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0
#endif

/*==================
 * DEFAULT FONT
 *==================*/

#define LV_FONT_DEFAULT &lv_font_montserrat_32

/*==================
 * Shared widgets / layouts / themes / features
 *==================*/

#include <ove/lv_conf_common.h>

#endif /* LV_CONF_H */
