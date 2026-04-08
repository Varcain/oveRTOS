/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Sim board initialisation -- starts the sim plugin framework
 * and transport.
 *
 * Called early during ove_app_run() → ove_board_init() when
 * CONFIG_OVE_SIM is enabled.
 *
 * Dashboard serving:
 *   POSIX / QEMU — external bridge (ove-dashboard-bridge.py)
 *   WASM         — Emscripten in-browser
 */

#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_transport.h"
#include "ove/sim/ove_sim_display.h"
#include "ove/sim/ove_sim_audio.h"
#include "ove/types.h"
#include "board_desc.h"

#include <stdio.h>
#include <string.h>

/* Audio defaults for boards that don't define I2S parameters. */
#ifndef OVE_AUDIO_I2S_SAMPLE_RATE
#define OVE_AUDIO_I2S_SAMPLE_RATE    16000
#endif
#ifndef OVE_AUDIO_I2S_CHANNELS
#define OVE_AUDIO_I2S_CHANNELS       1
#endif
#ifndef OVE_AUDIO_I2S_BIT_DEPTH
#define OVE_AUDIO_I2S_BIT_DEPTH      16
#endif
#ifndef OVE_AUDIO_I2S_BUFFER_SAMPLES
#define OVE_AUDIO_I2S_BUFFER_SAMPLES 512
#endif

/* Forward declarations for registration helpers. */
extern int ove_sim_display_register(uint16_t width, uint16_t height,
				    enum ove_sim_color_fmt fmt);
extern int ove_sim_audio_register(uint32_t sample_rate, uint16_t channels,
				  uint16_t bit_depth,
				  uint32_t buffer_frames);

/* ── Transport instance ────────────────────────────────────────────── */

static struct ove_sim_transport transport;

/* ── Sim board init ────────────────────────────────────────────────── */

int ove_sim_board_init(void)
{
	int ret;

	/* 1. Create transport. */
#if defined(__EMSCRIPTEN__)
	extern int ove_sim_transport_wasm_create(struct ove_sim_transport *t);
	ret = ove_sim_transport_wasm_create(&transport);
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
	ret = ove_sim_transport_shm_guest_create(&transport);
#else
	extern int ove_sim_transport_shm_local_create(
		struct ove_sim_transport *t);
	ret = ove_sim_transport_shm_local_create(&transport);
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
				       OVE_SIM_COLOR_XRGB8888);
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

#if defined(__EMSCRIPTEN__)
	/* WASM: start a command pump thread that drains the JS→C command
	 * queue and dispatches to plugins (audio inject, etc.). */
	{
		extern int ove_sim_wasm_cmd_pump_start(
			struct ove_sim_transport *t);
		ove_sim_wasm_cmd_pump_start(&transport);
	}
#endif

	printf("[sim] Simulation framework initialised (%d plugins)\n",
	       ove_sim_plugin_count());
	fflush(stdout);

	return OVE_OK;
}
