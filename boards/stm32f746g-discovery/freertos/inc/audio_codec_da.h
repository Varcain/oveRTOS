/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef INC_AUDIO_CODEC_H_
#define INC_AUDIO_CODEC_H_

#include <stdint.h>

/* Forward declaration */
struct audio_codec;

/* Audio codec driver structure */
struct audio_codec {
	/* Initialize the audio codec hardware */
	void (*init)(void);
};

/* Public API */

/**
 * @brief Set the audio codec driver implementation
 * @param codec Pointer to codec structure (must not be NULL)
 * @return 1 if driver was set successfully, 0 otherwise
 */
int audio_codec_set_driver(struct audio_codec *codec);

/**
 * @brief Check if audio codec driver is initialized
 * @return 1 if driver is set and ready, 0 otherwise
 */
int audio_codec_is_initialized(void);

/**
 * @brief Initialize audio codec hardware
 * @note Must call audio_codec_set_driver() before this function
 */
void audio_codec_init(void);

#endif /* INC_AUDIO_CODEC_H_ */
