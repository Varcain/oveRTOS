/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file audio.hpp
 * @brief Audio graph engine C++ wrappers
 */

#pragma once

#include <ove/audio.h>
#include <ove/audio_device.h>
#include <ove/types.hpp>

#ifdef CONFIG_OVE_AUDIO

namespace ove {
namespace audio {

/**
 * @brief C++ wrapper around ove_audio_graph.
 *
 * Provides RAII-style init/deinit and forwarding to the C graph API.
 */
class Graph {
public:
	Graph() : initialized_(false) {}

	~Graph() {
		if (initialized_)
			ove_audio_graph_deinit(&g_);
	}

	[[nodiscard]] int init(unsigned int frames_per_period) {
		int ret = ove_audio_graph_init(&g_, frames_per_period);
		if (ret == OVE_OK)
			initialized_ = true;
		return ret;
	}

	[[nodiscard]] int add_node(const struct ove_audio_node_ops *ops,
				   void *ctx, const char *name,
				   enum ove_audio_node_type type) {
		return ove_audio_graph_add_node(&g_, ops, ctx, name, type);
	}

	[[nodiscard]] int connect(unsigned int from, unsigned int to) {
		return ove_audio_graph_connect(&g_, from, to);
	}

	[[nodiscard]] int build() {
		return ove_audio_graph_build(&g_);
	}

	[[nodiscard]] int start() {
		return ove_audio_graph_start(&g_);
	}

	[[nodiscard]] int stop() {
		return ove_audio_graph_stop(&g_);
	}

	[[nodiscard]] int process() {
		return ove_audio_graph_process(&g_);
	}

	[[nodiscard]] int get_stats(struct ove_audio_graph_stats *stats) const {
		return ove_audio_graph_get_stats(&g_, stats);
	}

	[[nodiscard]] int device_source(const struct ove_audio_device_cfg *cfg,
					const char *name) {
		return ove_audio_device_source(&g_, cfg, name);
	}

	[[nodiscard]] int device_sink(const struct ove_audio_device_cfg *cfg,
				      const char *name) {
		return ove_audio_device_sink(&g_, cfg, name);
	}

	struct ove_audio_graph *raw() { return &g_; }

private:
	struct ove_audio_graph g_;
	bool initialized_;
};

} /* namespace audio */
} /* namespace ove */

#endif /* CONFIG_OVE_AUDIO */
