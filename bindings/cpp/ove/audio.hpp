/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file audio.hpp
 * @brief I2S audio streaming control functions
 */

#pragma once

#include <ove/audio.h>
#include <ove/types.hpp>

#ifdef CONFIG_OVE_AUDIO

namespace ove {

/**
 * @namespace ove::audio
 * @brief Thin C++ wrappers around the oveRTOS audio streaming API.
 *
 * Available when `CONFIG_OVE_AUDIO` is enabled.
 */
namespace audio {

/**
 * @brief Initialises the audio subsystem and registers a processing callback.
 * @param[in] cfg       Pointer to the audio configuration structure.
 * @param[in] cb        Callback invoked when an audio buffer needs processing.
 * @param[in] user_data Opaque pointer forwarded to the callback.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int init(const struct ove_audio_config *cfg,
			       ove_audio_process_fn cb,
			       void *user_data) {
	return ove_audio_init(cfg, cb, user_data);
}

/**
 * @brief Starts audio streaming.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int start() {
	return ove_audio_start();
}

/**
 * @brief Stops audio streaming.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int stop() {
	return ove_audio_stop();
}

/**
 * @brief Pauses audio streaming without tearing down the hardware.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int pause() {
	return ove_audio_pause();
}

/**
 * @brief Resumes audio streaming after a pause.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int resume() {
	return ove_audio_resume();
}

/**
 * @brief Deinitialises the audio subsystem and releases hardware resources.
 */
inline void deinit() {
	ove_audio_deinit();
}

} /* namespace audio */

} // namespace ove

#endif /* CONFIG_OVE_AUDIO */
