/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_audio Simulation Audio Plugin
 * @brief Audio plugin interface for PCM capture and injection.
 *
 * The audio plugin captures PCM output from the audio graph engine
 * and streams it to the web dashboard.  It can also receive audio
 * input from the dashboard (microphone, file) and inject it into
 * the firmware's audio source.
 * @{
 */

#ifndef OVE_SIM_AUDIO_H
#define OVE_SIM_AUDIO_H

#include "ove_sim_plugin.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Audio event types ─────────────────────────────────────────────── */

/** @brief Event: PCM samples are ready (firmware -> dashboard). */
#define OVE_SIM_AUDIO_EVT_SAMPLES   0

/** @brief Event: audio format changed. */
#define OVE_SIM_AUDIO_EVT_FMT       1

/* ── Audio command types ───────────────────────────────────────────── */

/** @brief Command: inject PCM samples (dashboard -> firmware). */
#define OVE_SIM_AUDIO_CMD_INJECT    0

/** @brief Command: set capture enable/disable. */
#define OVE_SIM_AUDIO_CMD_CAPTURE   1

/* ── Audio sample format ───────────────────────────────────────────── */

/** @brief Audio sample format descriptor for the sim transport. */
struct ove_sim_audio_fmt {
	uint32_t sample_rate;  /**< Sample rate in Hz. */
	uint16_t channels;     /**< Number of channels. */
	uint16_t bit_depth;    /**< Bits per sample (16, 24, 32). */
};

/* ── Audio plugin configuration ────────────────────────────────────── */

/**
 * @brief Audio plugin configuration (from board.yaml).
 */
struct ove_sim_audio_cfg {
	struct ove_sim_audio_fmt fmt;         /**< Default audio format. */
	uint32_t                 buffer_frames; /**< Frames per buffer period. */
};

/* ── Built-in audio plugin ─────────────────────────────────────────── */

/**
 * @brief Get the built-in audio plugin ops.
 *
 * The returned ops provide a sim audio device.
 * PCM output is streamed to the dashboard; PCM input is received
 * from the dashboard.
 *
 * @return Pointer to the static audio plugin ops.
 */
const struct ove_sim_plugin_ops *ove_sim_audio_builtin_ops(void);

/**
 * @brief Push PCM output samples to the dashboard.
 *
 * Called by the sim audio graph sink node each processing cycle.
 *
 * @param[in] samples  Interleaved PCM sample data.
 * @param[in] len      Byte length of @p samples.
 * @param[in] fmt      Format of the samples.
 */
void ove_sim_audio_push_output(const void *samples, size_t len,
			       const struct ove_sim_audio_fmt *fmt);

/**
 * @brief Pull PCM input samples from the dashboard.
 *
 * Called by the sim audio graph source node each processing cycle.
 *
 * @param[out] samples  Buffer to fill with interleaved PCM data.
 * @param[in]  len      Byte length of @p samples buffer.
 * @param[in]  fmt      Expected format.
 * @return Number of bytes actually filled (0 if no data available).
 */
size_t ove_sim_audio_pull_input(void *samples, size_t len,
				const struct ove_sim_audio_fmt *fmt);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_AUDIO_H */
