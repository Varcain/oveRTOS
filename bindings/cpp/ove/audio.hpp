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

namespace ove::audio
{

/**
 * @brief Build an audio device config for the I2S transport.
 *
 * Mirrors `ove::audio::device_cfg_i2s` in the Rust binding and
 * `ove.audio.deviceCfgI2s` in the Zig binding.  Both of those need
 * pointer-cast tricks to write into the anonymous union of
 * `ove_audio_device_cfg`; C++ supports anonymous unions natively
 * (`cfg.i2s.input_device`) so the equivalent here is straightforward.
 *
 * Sample format defaults to S16.  Callers that need TDM slot masking
 * can set `cfg.i2s.slot_mask` on the returned value.
 *
 * @param sample_rate   Sample rate in Hz (e.g. 16000, 48000).
 * @param channels      Channel count (1 = mono, 2 = stereo).
 * @param input_device  Input device selector (e.g. line-in, DMIC index).
 * @return Filled-in `ove_audio_device_cfg` ready to pass to
 *         `Graph::device_source` / `Graph::device_sink`.
 */
inline struct ove_audio_device_cfg device_cfg_i2s(uint32_t sample_rate, uint32_t channels,
						  uint32_t input_device)
{
	struct ove_audio_device_cfg cfg {
	};
	cfg.transport = OVE_AUDIO_TRANSPORT_I2S;
	cfg.fmt.sample_rate = sample_rate;
	cfg.fmt.channels = channels;
	cfg.fmt.sample_fmt = OVE_AUDIO_FMT_S16;
	cfg.i2s.input_device = input_device;
	return cfg;
}

/**
 * @brief C++ wrapper around ove_audio_graph.
 *
 * Provides RAII-style init/deinit and forwarding to the C graph API.
 */
class Graph
{
      public:
	Graph() : initialized_(false)
	{
	}

	~Graph() noexcept
	{
		if (initialized_)
			ove_audio_graph_deinit(&g_);
	}

	/// Initialise the audio graph with the given period size.
	[[nodiscard]] int init(unsigned int frames_per_period)
	{
		int ret = ove_audio_graph_init(&g_, frames_per_period);
		if (ret == OVE_OK)
			initialized_ = true;
		return ret;
	}

	/// Initialise the graph in a way that works for both heap and zero-heap
	/// builds.  In zero-heap mode, emits a per-call-site static backing array
	/// sized by `OVE_AUDIO_GRAPH_STORAGE_BYTES(Nodes, Frames, Channels,
	/// SampleBytes)` and attaches it via `set_buf_storage()`.  Mirrors the
	/// C `ove_audio_graph_create` macro — callers can use `.init()` + manual
	/// `set_buf_storage()` for dynamic sizing, or this helper for the common
	/// statically-known case.
	///
	/// All template arguments must be compile-time constants.
	template <unsigned Nodes, unsigned Frames, unsigned Channels = 1, unsigned SampleBytes = 2>
	[[nodiscard]] int create()
	{
		int ret = ove_audio_graph_init(&g_, Frames);
		if (ret != OVE_OK)
			return ret;
		initialized_ = true;
#ifdef CONFIG_OVE_ZERO_HEAP
		alignas(4) static unsigned char storage[OVE_AUDIO_GRAPH_STORAGE_BYTES(
			Nodes, Frames, Channels, SampleBytes)];
		ret = ove_audio_graph_set_buf_storage(&g_, storage, sizeof(storage));
#endif
		return ret;
	}

	/// Add a processing node to the graph.
	[[nodiscard]] int add_node(const struct ove_audio_node_ops *ops, void *ctx,
				   const char *name, enum ove_audio_node_type type)
	{
		return ove_audio_graph_add_node(&g_, ops, ctx, name, type);
	}

	/// Connect two nodes (output of @p from to input of @p to).
	[[nodiscard]] int connect(unsigned int from, unsigned int to)
	{
		return ove_audio_graph_connect(&g_, from, to);
	}

	/// Finalize the graph topology.
	[[nodiscard]] int build()
	{
		return ove_audio_graph_build(&g_);
	}

	/// Start audio processing.
	[[nodiscard]] int start()
	{
		return ove_audio_graph_start(&g_);
	}

	/// Stop audio processing.
	[[nodiscard]] int stop()
	{
		return ove_audio_graph_stop(&g_);
	}

	/// Run one processing period through the graph.
	[[nodiscard]] int process()
	{
		return ove_audio_graph_process(&g_);
	}

	/// Query runtime statistics.
	[[nodiscard]] int get_stats(struct ove_audio_graph_stats *stats) const
	{
		return ove_audio_graph_get_stats(&g_, stats);
	}

	/// Add an audio input device as a source node.
	[[nodiscard]] int device_source(const struct ove_audio_device_cfg *cfg, const char *name)
	{
		return ove_audio_device_source(&g_, cfg, name);
	}

	/// Add an audio output device as a sink node.
	[[nodiscard]] int device_sink(const struct ove_audio_device_cfg *cfg, const char *name)
	{
		return ove_audio_device_sink(&g_, cfg, name);
	}

	/// Access the underlying C graph struct.
	struct ove_audio_graph *raw()
	{
		return &g_;
	}

	/// Register a custom processor node using a typed C++ object.
	///
	/// `T` must have: `int process(const ove_audio_buf *in, ove_audio_buf *out)`
	/// The binding layer generates trampolines — no raw function pointers needed.
	template <typename T> [[nodiscard]] int add_processor(T &node, const char *name)
	{
		static const struct ove_audio_node_ops ops = {
			/* configure */
			[](void *, const struct ove_audio_fmt *in_f,
			   struct ove_audio_fmt *out_f) -> int {
				if (in_f && out_f)
					*out_f = *in_f;
				return OVE_OK;
			},
			/* start */ nullptr,
			/* stop */ nullptr,
			/* process */
			[](void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
				-> int { return static_cast<T *>(ctx)->process(in, out); },
			/* destroy */ nullptr,
		};
		return ove_audio_graph_add_node(&g_, &ops, &node, name, OVE_AUDIO_NODE_PROCESSOR);
	}

      private:
	struct ove_audio_graph g_;
	bool initialized_;
};

} /* namespace ove::audio */

#endif /* CONFIG_OVE_AUDIO */
