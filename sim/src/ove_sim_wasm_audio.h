/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM audio ring buffers.
 *
 * Playback: firmware writes PCM → playback ring → JS AudioWorklet → speaker
 * Capture:  JS AudioWorklet → capture ring → firmware reads PCM
 *
 * Both rings live in the WASM SharedArrayBuffer heap so JS and C
 * can access them without copies.  Uses the common ove_sim_audio_ring
 * struct for layout compatibility with POSIX and QEMU transports.
 */

#ifndef OVE_SIM_WASM_AUDIO_H
#define OVE_SIM_WASM_AUDIO_H

#include "ove_sim_audio_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ove_sim_wasm_audio {
	struct ove_sim_audio_ring playback; /* firmware → speaker */
	struct ove_sim_audio_ring capture;  /* mic → firmware */
};

extern struct ove_sim_wasm_audio ove_wasm_audio;

/* Exported accessors for JS */
void *ove_wasm_audio_get_playback_ptr(void);
void *ove_wasm_audio_get_capture_ptr(void);
uint32_t ove_wasm_audio_playback_available(void);
uint32_t ove_wasm_audio_capture_available(void);

/* C-side write/read used by sim_audio.c */
void ove_wasm_audio_playback_write(const void *samples, uint32_t len);
size_t ove_wasm_audio_capture_read(void *samples, uint32_t len);

/* Called from JS to configure format after getUserMedia */
void ove_wasm_audio_set_capture_fmt(uint32_t rate, uint16_t ch, uint16_t bits);
void ove_wasm_audio_set_playback_fmt(uint32_t rate, uint16_t ch, uint16_t bits);

#ifdef __cplusplus
}
#endif

#endif /* OVE_SIM_WASM_AUDIO_H */
