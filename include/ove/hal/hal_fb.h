/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_FB_H
#define OVE_HAL_FB_H

/**
 * @defgroup ove_hal_fb HAL framebuffer interface
 * @brief The board-supplied framebuffer primitives the @ref ove_fb layer drives.
 *
 * A board backend (qemu_fb.c on an500, fb_port.c on STM32F746, null_fb.c on
 * an521) implements these; the portable @ref ove_fb layer forwards to them and
 * owns the dirty flag.
 * @{
 */

#include "ove/fb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bring up the display + framebuffer memory. OVE_OK or a negative error. */
int ove_hal_fb_init(void);

/** @brief Report the framebuffer geometry into @p info. */
int ove_hal_fb_get_info(struct ove_fb_info *info);

/** @brief The linear pixel buffer the display scans out / is pushed from. */
void *ove_hal_fb_buffer(void);

/** @brief Push the current buffer contents to the physical display (an500: shm
 *  write; F746: no-op, LTDC scans SDRAM continuously). */
void ove_hal_fb_present(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_FB_H */
