/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_FB_H
#define OVE_FB_H

/**
 * @defgroup ove_fb Framebuffer
 * @brief A minimal raw linear framebuffer HAL.
 *
 * One display, one linear RGB565 buffer. The portable layer here tracks the
 * dirty flag and forwards to the board HAL (@ref ove_hal_fb.h); a board backend
 * supplies the buffer (an500: RAM pushed to a host viewer; STM32F746: the LTDC
 * scanout buffer in SDRAM). The Linux personality's /dev/fb0 class driver is the
 * primary consumer, but a native app may use it too.
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Pixel format (only RGB565 for now). */
typedef enum {
	OVE_FB_FMT_RGB565 = 0,
} ove_fb_fmt_t;

/** Geometry + memory extent of the framebuffer. */
struct ove_fb_info {
	uint16_t width;	       /**< Visible width in pixels. */
	uint16_t height;       /**< Visible height in pixels. */
	uint16_t stride_bytes; /**< Bytes per scanline (width * bytes-per-pixel). */
	ove_fb_fmt_t fmt;      /**< Pixel format. */
	uint32_t smem_len;     /**< Total buffer size in bytes (stride * height). */
};

/**
 * @brief Bring up the framebuffer backend (once, on the coordinator thread).
 * @return OVE_OK, or a negative error code if the board has no display.
 */
int ove_fb_init(void);

/** @brief Fill @p info with the framebuffer geometry. Returns OVE_OK or an error. */
int ove_fb_get_info(struct ove_fb_info *info);

/** @brief The linear pixel buffer (write pixels here, then @ref ove_fb_present). */
void *ove_fb_get_buffer(void);

/**
 * @brief Publish and present an updated rectangle of the framebuffer.
 *
 * The caller owns dirty-region coalescing. The board backend performs any cache
 * maintenance needed by scanout and presents the rectangle. Backends that only
 * support whole-frame updates may treat @p x/y/w/h as advisory.
 */
void ove_fb_present(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_FB_H */
