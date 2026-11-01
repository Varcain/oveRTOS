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
 * headless test binary. These factories register real graph nodes with
 * inert operations so retained metadata and registration errors follow
 * the common graph path without opening audio hardware.
 */

#include "ove/ove.h"
#include "ove/audio_device.h"
#include <string.h>

static int inert_configure(void *ctx, const struct ove_audio_fmt *in, struct ove_audio_fmt *out)
{
	(void)ctx;
	(void)in;
	(void)out;
	return OVE_ERR_NOT_SUPPORTED;
}

static int inert_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	(void)ctx;
	(void)in;
	(void)out;
	return OVE_ERR_NOT_SUPPORTED;
}

static const struct ove_audio_node_ops inert_ops = {
	.configure = inert_configure,
	.process = inert_process,
};
int ove_audio_device_source_n(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			      const char *name, size_t name_len)
{
	if (!cfg)
		return OVE_ERR_INVALID_PARAM;
	return ove_audio_graph_add_node_n(g, &inert_ops, NULL, name, name_len,
					  OVE_AUDIO_NODE_SOURCE);
}
int ove_audio_device_sink_n(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			    const char *name, size_t name_len)
{
	if (!cfg)
		return OVE_ERR_INVALID_PARAM;
	return ove_audio_graph_add_node_n(g, &inert_ops, NULL, name, name_len, OVE_AUDIO_NODE_SINK);
}

int ove_audio_device_source(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	return name ? ove_audio_device_source_n(g, cfg, name, strlen(name)) : OVE_ERR_INVALID_PARAM;
}
int ove_audio_device_sink(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	return name ? ove_audio_device_sink_n(g, cfg, name, strlen(name)) : OVE_ERR_INVALID_PARAM;
}
