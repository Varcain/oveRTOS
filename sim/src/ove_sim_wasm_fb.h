/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared framebuffer for WASM mode.
 *
 * The firmware writes XRGB8888 frames into a fixed buffer in the WASM
 * heap.  The JS main thread reads via HEAPU8 on requestAnimationFrame.
 * Synchronisation is a simple atomic frame counter — JS detects new
 * frames by comparing its last-seen counter with the current value.
 */

#ifndef OVE_SIM_WASM_FB_H
#define OVE_SIM_WASM_FB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max display: 480x272 XRGB8888 = 522240 bytes. */
#define OVE_SIM_WASM_FB_MAX (480 * 272 * 4)

struct ove_sim_wasm_fb {
	volatile uint32_t frame_seq;     /* incremented each new frame */
	uint16_t          width;
	uint16_t          height;
	uint32_t          size;          /* bytes of pixel data */
	uint8_t           pixels[OVE_SIM_WASM_FB_MAX];
};

/* Global instance — address exported to JS. */
extern struct ove_sim_wasm_fb ove_wasm_fb;

/* Write a frame (called from sim_lvgl.c via transport). */
void ove_sim_wasm_fb_write(const uint8_t *pixels, uint32_t size,
			   uint16_t width, uint16_t height);

#ifdef __cplusplus
}
#endif

#endif /* OVE_SIM_WASM_FB_H */
