/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifdef __EMSCRIPTEN__

#include "ove_sim_wasm_audio.h"
#include <emscripten.h>
#include <string.h>

/* Global audio state in WASM heap (SharedArrayBuffer). */
struct ove_sim_wasm_audio ove_wasm_audio = {
	.playback = { .size = OVE_SIM_AUDIO_RING_SIZE },
	.capture  = { .size = OVE_SIM_AUDIO_RING_SIZE },
};

/* ── Exported accessors for JS ─────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE
void *ove_wasm_audio_get_playback_ptr(void)
{
	return &ove_wasm_audio.playback;
}

EMSCRIPTEN_KEEPALIVE
void *ove_wasm_audio_get_capture_ptr(void)
{
	return &ove_wasm_audio.capture;
}

EMSCRIPTEN_KEEPALIVE
uint32_t ove_wasm_audio_playback_available(void)
{
	return ove_sim_ring_avail_atomic(&ove_wasm_audio.playback);
}

EMSCRIPTEN_KEEPALIVE
uint32_t ove_wasm_audio_capture_available(void)
{
	return ove_sim_ring_avail_atomic(&ove_wasm_audio.capture);
}

EMSCRIPTEN_KEEPALIVE
void ove_wasm_audio_set_capture_fmt(uint32_t rate, uint16_t ch, uint16_t bits)
{
	ove_wasm_audio.capture.sample_rate = rate;
	ove_wasm_audio.capture.channels = ch;
	ove_wasm_audio.capture.bit_depth = bits;
	ove_wasm_audio.capture.size = OVE_SIM_AUDIO_RING_SIZE;
}

EMSCRIPTEN_KEEPALIVE
void ove_wasm_audio_set_playback_fmt(uint32_t rate, uint16_t ch, uint16_t bits)
{
	ove_wasm_audio.playback.sample_rate = rate;
	ove_wasm_audio.playback.channels = ch;
	ove_wasm_audio.playback.bit_depth = bits;
	ove_wasm_audio.playback.size = OVE_SIM_AUDIO_RING_SIZE;
}

/* ── C-side API (called from sim_audio.c graph nodes) ──────────────── */

void ove_wasm_audio_playback_write(const void *samples, uint32_t len)
{
	ove_sim_ring_write_atomic(&ove_wasm_audio.playback,
				  samples, len);
}

size_t ove_wasm_audio_capture_read(void *samples, uint32_t len)
{
	uint32_t got = ove_sim_ring_read_atomic(&ove_wasm_audio.capture,
						samples, len);
	/* Zero-fill remainder if mic hasn't provided enough. */
	if (got < len)
		memset((uint8_t *)samples + got, 0, len - got);
	return got;
}

#endif /* __EMSCRIPTEN__ */
