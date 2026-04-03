/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_display Simulation Display Plugin
 * @brief Display plugin interface for framebuffer capture and streaming.
 *
 * The display plugin captures LVGL framebuffer flushes and streams them
 * to the web dashboard via WebSocket binary frames.
 * @{
 */

#ifndef OVE_SIM_DISPLAY_H
#define OVE_SIM_DISPLAY_H

#include "ove_sim_plugin.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Display event types ───────────────────────────────────────────── */

/** @brief Event: a new frame is ready to be sent to the dashboard. */
#define OVE_SIM_DISPLAY_EVT_FRAME   0

/* ── Display command types ─────────────────────────────────────────── */

/** @brief Command: request a full-frame refresh. */
#define OVE_SIM_DISPLAY_CMD_REFRESH 0

/* ── Display plugin sub-interface ──────────────────────────────────── */

/**
 * @brief Extended ops for display plugins.
 */
struct ove_sim_display_ops {
	struct ove_sim_plugin_ops base;

	/**
	 * @brief Called when a new framebuffer region has been flushed.
	 *
	 * @param[in] ctx     Plugin-private context.
	 * @param[in] fb      Pixel data (format per @p color_fmt).
	 * @param[in] fb_len  Size of @p fb in bytes.
	 * @param[in] x1      Left edge of the dirty rectangle.
	 * @param[in] y1      Top edge of the dirty rectangle.
	 * @param[in] x2      Right edge of the dirty rectangle (inclusive).
	 * @param[in] y2      Bottom edge of the dirty rectangle (inclusive).
	 */
	void (*flush_cb)(void *ctx, const void *fb, size_t fb_len,
			 uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
};

/* ── Display plugin context ────────────────────────────────────────── */

/** @brief Pixel colour format. */
enum ove_sim_color_fmt {
	OVE_SIM_COLOR_RGB565,  /**< 16-bit RGB565. */
	OVE_SIM_COLOR_RGB888,  /**< 24-bit RGB888. */
	OVE_SIM_COLOR_XRGB8888, /**< 32-bit XRGB8888. */
};

/**
 * @brief Display plugin configuration (from board.yaml).
 */
struct ove_sim_display_cfg {
	uint16_t                width;     /**< Display width in pixels. */
	uint16_t                height;    /**< Display height in pixels. */
	enum ove_sim_color_fmt  color_fmt; /**< Pixel format. */
};

/* ── Built-in display plugin ───────────────────────────────────────── */

/**
 * @brief Get the built-in display plugin ops.
 *
 * The returned ops capture LVGL framebuffer flushes and emit
 * FRAME events via the transport.
 *
 * @return Pointer to the static display plugin ops.
 */
const struct ove_sim_display_ops *ove_sim_display_builtin_ops(void);

/**
 * @brief Notify the display plugin of a framebuffer flush.
 *
 * Called by the sim LVGL integration when LVGL flushes a dirty region.
 *
 * @param[in] fb      Pixel data.
 * @param[in] fb_len  Byte length of pixel data.
 * @param[in] x1      Left edge.
 * @param[in] y1      Top edge.
 * @param[in] x2      Right edge (inclusive).
 * @param[in] y2      Bottom edge (inclusive).
 */
void ove_sim_display_flush(const void *fb, size_t fb_len,
			   uint16_t x1, uint16_t y1,
			   uint16_t x2, uint16_t y2);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_DISPLAY_H */
