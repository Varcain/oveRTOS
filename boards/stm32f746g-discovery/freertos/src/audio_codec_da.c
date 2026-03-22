/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "audio_codec_da.h"
#include <stddef.h>

static const struct audio_codec *driver = NULL;

int audio_codec_set_driver(struct audio_codec *codec)
{
	if (codec == NULL) {
		return 0;
	}

	/* Validate that required operations are provided */
	if (codec->init == NULL) {
		return 0;
	}

	driver = codec;
	return 1;
}

int audio_codec_is_initialized(void)
{
	return (driver != NULL) ? 1 : 0;
}

void audio_codec_init(void)
{
	if (driver != NULL && driver->init != NULL) {
		driver->init();
	}
}
