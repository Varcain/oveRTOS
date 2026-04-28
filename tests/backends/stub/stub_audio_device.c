/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Minimal stubs for audio device factories used by the Rust ove_stub
 * test library.  The real implementations live in sim/hal/sim_audio.c,
 * which pulls in miniaudio/portaudio and is therefore unsuitable for a
 * headless test binary.  These stubs only validate arguments and
 * return a dummy node index so the Rust `Graph::device_source` /
 * `Graph::device_sink` wrappers can exercise both their Ok and Err
 * branches.
 */

#include "ove/ove.h"
#include "ove/audio_device.h"

int ove_audio_device_source(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	if (!g || !cfg || !name || name[0] == '\0')
		return OVE_ERR_INVALID_PARAM;
	return 0;
}

int ove_audio_device_sink(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	if (!g || !cfg || !name || name[0] == '\0')
		return OVE_ERR_INVALID_PARAM;
	return 0;
}
