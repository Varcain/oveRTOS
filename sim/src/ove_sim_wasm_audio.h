/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared audio ring buffers for WASM mode.
 *
 * Playback: firmware writes PCM → playback ring → JS AudioWorklet → speaker
 * Capture:  JS AudioWorklet → capture ring → firmware reads PCM
 *
 * Both rings live in the WASM SharedArrayBuffer heap so JS and C
 * can access them without copies.  Synchronization uses atomic
 * read/write positions (SPSC: one producer, one consumer per ring).
 */

#ifndef OVE_SIM_WASM_AUDIO_H
#define OVE_SIM_WASM_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ring size: 16KB per direction.  At 44100 Hz / 16-bit / mono = 88200 B/s,
 * this gives ~180ms of buffer — enough for AudioWorklet's 128-frame chunks.
 */
#define OVE_SIM_WASM_AUDIO_RING_SIZE (1u << 14) /* 16384 bytes */

struct ove_sim_wasm_audio_ring {
	volatile uint32_t write_pos;
	volatile uint32_t read_pos;
	uint32_t          sample_rate;
	uint16_t          channels;
	uint16_t          bit_depth;
	uint8_t           buf[OVE_SIM_WASM_AUDIO_RING_SIZE];
};

struct ove_sim_wasm_audio {
	struct ove_sim_wasm_audio_ring playback; /* firmware → speaker */
	struct ove_sim_wasm_audio_ring capture;  /* mic → firmware */
};

extern struct ove_sim_wasm_audio ove_wasm_audio;

/* Exported accessors for JS */
void    *ove_wasm_audio_get_playback_ptr(void);
void    *ove_wasm_audio_get_capture_ptr(void);
uint32_t ove_wasm_audio_playback_available(void);
uint32_t ove_wasm_audio_capture_available(void);

/* C-side write/read used by sim_audio.c */
void   ove_wasm_audio_playback_write(const void *samples, uint32_t len);
size_t ove_wasm_audio_capture_read(void *samples, uint32_t len);

/* Called from JS to configure format after getUserMedia */
void ove_wasm_audio_set_capture_fmt(uint32_t rate, uint16_t ch, uint16_t bits);
void ove_wasm_audio_set_playback_fmt(uint32_t rate, uint16_t ch, uint16_t bits);

#ifdef __cplusplus
}
#endif

#endif /* OVE_SIM_WASM_AUDIO_H */
