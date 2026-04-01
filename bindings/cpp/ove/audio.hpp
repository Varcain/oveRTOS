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

	/// Initialise the audio graph with the given period size.
	[[nodiscard]] int init(unsigned int frames_per_period) {
		int ret = ove_audio_graph_init(&g_, frames_per_period);
		if (ret == OVE_OK)
			initialized_ = true;
		return ret;
	}

	/// Add a processing node to the graph.
	[[nodiscard]] int add_node(const struct ove_audio_node_ops *ops,
				   void *ctx, const char *name,
				   enum ove_audio_node_type type) {
		return ove_audio_graph_add_node(&g_, ops, ctx, name, type);
	}

	/// Connect two nodes (output of @p from to input of @p to).
	[[nodiscard]] int connect(unsigned int from, unsigned int to) {
		return ove_audio_graph_connect(&g_, from, to);
	}

	/// Finalize the graph topology.
	[[nodiscard]] int build() {
		return ove_audio_graph_build(&g_);
	}

	/// Start audio processing.
	[[nodiscard]] int start() {
		return ove_audio_graph_start(&g_);
	}

	/// Stop audio processing.
	[[nodiscard]] int stop() {
		return ove_audio_graph_stop(&g_);
	}

	/// Run one processing period through the graph.
	[[nodiscard]] int process() {
		return ove_audio_graph_process(&g_);
	}

	/// Query runtime statistics.
	[[nodiscard]] int get_stats(struct ove_audio_graph_stats *stats) const {
		return ove_audio_graph_get_stats(&g_, stats);
	}

	/// Add an audio input device as a source node.
	[[nodiscard]] int device_source(const struct ove_audio_device_cfg *cfg,
					const char *name) {
		return ove_audio_device_source(&g_, cfg, name);
	}

	/// Add an audio output device as a sink node.
	[[nodiscard]] int device_sink(const struct ove_audio_device_cfg *cfg,
				      const char *name) {
		return ove_audio_device_sink(&g_, cfg, name);
	}

	/// Access the underlying C graph struct.
	struct ove_audio_graph *raw() { return &g_; }

	/// Register a custom processor node using a typed C++ object.
	///
	/// `T` must have: `int process(const ove_audio_buf *in, ove_audio_buf *out)`
	/// The binding layer generates trampolines — no raw function pointers needed.
	template<typename T>
	[[nodiscard]] int add_processor(T &node, const char *name) {
		static const struct ove_audio_node_ops ops = {
			/* configure */
			[](void *, const struct ove_audio_fmt *in_f,
			   struct ove_audio_fmt *out_f) -> int {
				if (in_f && out_f) *out_f = *in_f;
				return OVE_OK;
			},
			/* start */  nullptr,
			/* stop */   nullptr,
			/* process */
			[](void *ctx, const struct ove_audio_buf *in,
			   struct ove_audio_buf *out) -> int {
				return static_cast<T*>(ctx)->process(in, out);
			},
			/* destroy */ nullptr,
		};
		return ove_audio_graph_add_node(&g_, &ops, &node, name,
						OVE_AUDIO_NODE_PROCESSOR);
	}

private:
	struct ove_audio_graph g_;
	bool initialized_;
};

} /* namespace audio */
} /* namespace ove */

#endif /* CONFIG_OVE_AUDIO */
