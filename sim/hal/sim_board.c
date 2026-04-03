/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Sim board initialisation -- starts the sim plugin framework,
 * transport, and WebSocket dashboard server.
 *
 * Called early during ove_app_run() → ove_board_init() when
 * CONFIG_OVE_SIM is enabled.
 */

#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_transport.h"
#include "ove/sim/ove_sim_display.h"
#include "ove/sim/ove_sim_audio.h"
#include "../src/ove_sim_ws.h"
#include "ove/types.h"
#include "board_desc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OVE_SIM_DASHBOARD_PORT
#define OVE_SIM_DASHBOARD_PORT 8080
#endif

/* Forward declarations for registration helpers. */
extern int ove_sim_display_register(uint16_t width, uint16_t height,
				    enum ove_sim_color_fmt fmt);
extern int ove_sim_audio_register(uint32_t sample_rate, uint16_t channels,
				  uint16_t bit_depth,
				  uint32_t buffer_frames);

/* ── Transport instance ────────────────────────────────────────────── */

static struct ove_sim_transport transport;

/* ── Dashboard path resolution ─────────────────────────────────────── */

static const char *resolve_dashboard_path(void)
{
	/* Try OVE_SIM_DASHBOARD_PATH env var first. */
	const char *env = getenv("OVE_SIM_DASHBOARD_PATH");
	if (env)
		return env;

	/* Try relative to OVE_DIR env var. */
	const char *ove_dir = getenv("OVE_DIR");
	if (ove_dir) {
		static char path[512];
		snprintf(path, sizeof(path), "%s/sim/dashboard", ove_dir);
		return path;
	}

	/* Fallback: relative to CWD. */
	return "sim/dashboard";
}

/* ── Sim board init ────────────────────────────────────────────────── */

int ove_sim_board_init(void)
{
	int ret;

	/* 1. Create transport. */
#ifdef __EMSCRIPTEN__
	extern int ove_sim_transport_wasm_create(struct ove_sim_transport *t);
	ret = ove_sim_transport_wasm_create(&transport);
#else
	ret = ove_sim_transport_direct_create(&transport);
#endif
	if (ret != OVE_OK) {
		fprintf(stderr, "[sim] Failed to create transport: %d\n", ret);
		return ret;
	}

	ret = ove_sim_transport_open(&transport, NULL);
	if (ret != OVE_OK) {
		fprintf(stderr, "[sim] Failed to open transport: %d\n", ret);
		return ret;
	}

	/* Set as global transport for all plugins. */
	ove_sim_set_transport(&transport);

	/* 2. Register built-in plugins. */
#ifdef CONFIG_OVE_LVGL
	ret = ove_sim_display_register(OVE_DISPLAY_WIDTH,
				       OVE_DISPLAY_HEIGHT,
				       OVE_SIM_COLOR_RGB565);
	if (ret < 0)
		fprintf(stderr, "[sim] Display plugin failed: %d\n", ret);
#endif

#ifdef CONFIG_OVE_AUDIO
	ret = ove_sim_audio_register(
		OVE_AUDIO_I2S_SAMPLE_RATE,
		OVE_AUDIO_I2S_CHANNELS,
		OVE_AUDIO_I2S_BIT_DEPTH,
		OVE_AUDIO_I2S_BUFFER_SAMPLES);
	if (ret < 0)
		fprintf(stderr, "[sim] Audio plugin failed: %d\n", ret);
#endif

	/* 3. Start the WebSocket server (POSIX only — WASM uses postMessage). */
#ifndef __EMSCRIPTEN__
	const char *dash_path = resolve_dashboard_path();
	ret = ove_sim_ws_start(OVE_SIM_DASHBOARD_PORT, dash_path);
	if (ret != OVE_OK)
		fprintf(stderr, "[sim] Dashboard failed to start: %d\n", ret);
#endif

	printf("[sim] Simulation framework initialised (%d plugins)\n",
	       ove_sim_plugin_count());
	fflush(stdout);

	return OVE_OK;
}
