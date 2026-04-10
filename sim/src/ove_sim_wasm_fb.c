/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifdef __EMSCRIPTEN__

#include "ove_sim_wasm_fb.h"
#include <emscripten.h>
#include <string.h>

/* Global framebuffer — lives in WASM linear memory (SharedArrayBuffer
 * when pthreads are enabled).  JS reads it via Module.HEAPU8. */
struct ove_sim_wasm_fb ove_wasm_fb;

/* ── Exported accessors for JS ─────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE
uint32_t ove_wasm_fb_get_seq(void)
{
	return ove_wasm_fb.frame_seq;
}

EMSCRIPTEN_KEEPALIVE
void *ove_wasm_fb_get_ptr(void)
{
	return &ove_wasm_fb;
}

EMSCRIPTEN_KEEPALIVE
uint16_t ove_wasm_fb_get_width(void)
{
	return ove_wasm_fb.width;
}

EMSCRIPTEN_KEEPALIVE
uint16_t ove_wasm_fb_get_height(void)
{
	return ove_wasm_fb.height;
}

EMSCRIPTEN_KEEPALIVE
void *ove_wasm_fb_get_pixels(void)
{
	return ove_wasm_fb.pixels;
}

EMSCRIPTEN_KEEPALIVE
uint32_t ove_wasm_fb_get_size(void)
{
	return ove_wasm_fb.size;
}

/* ── Write a new frame (called from firmware thread) ───────────────── */

void ove_sim_wasm_fb_write(const uint8_t *pixels, uint32_t size,
			   uint16_t width, uint16_t height)
{
	if (size > OVE_SIM_WASM_FB_MAX)
		return;
	memcpy(ove_wasm_fb.pixels, pixels, size);
	ove_wasm_fb.width = width;
	ove_wasm_fb.height = height;
	ove_wasm_fb.size = size;
	__sync_fetch_and_add(&ove_wasm_fb.frame_seq, 1);
}

#endif /* __EMSCRIPTEN__ */
