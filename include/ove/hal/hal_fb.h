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
 * an521) implements these; the portable @ref ove_fb layer is a thin forwarder.
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

/** @brief Publish/present the supplied dirty rectangle (an500 may push the
 * whole buffer; continuously scanned non-cacheable F746 buffers are a no-op). */
void ove_hal_fb_present(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_FB_H */
