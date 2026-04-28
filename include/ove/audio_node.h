/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
 * @defgroup ove_audio_node Audio node types and built-in factories
 * @ingroup ove_audio
 * @brief Core types for audio nodes, formats, buffers, and built-in
 *        processor node factories.
 *
 * Defines the fundamental data types shared by every node in the audio
 * graph: sample format, format descriptor, audio buffer, node vtable,
 * and node descriptor.  Also provides a small set of ready-made
 * processor node factories (format converter, channel mapper, gain,
 * and tap observer).
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @{
 */

#ifndef OVE_AUDIO_NODE_H
#define OVE_AUDIO_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ove/storage.h"

/** @brief Maximum number of audio channels supported by the channel-map node. */
#define OVE_AUDIO_MAX_CHANNELS 8

/* ── Sample format ──────────────────────────────────────────────── */

/**
 * @brief PCM sample format tag.
 *
 * Identifies the numeric type and bit-depth of each audio sample.
 */
enum ove_audio_sample_fmt {
	OVE_AUDIO_FMT_S16, /**< @brief Signed 16-bit integer (int16_t). */
	OVE_AUDIO_FMT_S32, /**< @brief Signed 32-bit integer (int32_t). */
	OVE_AUDIO_FMT_F32, /**< @brief 32-bit IEEE 754 float. */
};

/**
 * @brief Return the size in bytes of one sample for a given format.
 *
 * @param[in] fmt  Sample format tag.
 * @return Byte size of one sample, or 0 for an unrecognised format.
 */
static inline unsigned int ove_audio_sample_size(enum ove_audio_sample_fmt fmt)
{
	switch (fmt) {
	case OVE_AUDIO_FMT_S16:
		return sizeof(int16_t);
	case OVE_AUDIO_FMT_S32:
		return sizeof(int32_t);
	case OVE_AUDIO_FMT_F32:
		return sizeof(float);
	default:
		return 0;
	}
}

/* ── Audio format descriptor ────────────────────────────────────── */

/**
 * @brief Complete audio stream format descriptor.
 *
 * Describes the sample rate, channel count, and sample encoding of an
 * audio stream.  Channels are always interleaved.
 */
struct ove_audio_fmt {
	unsigned int sample_rate;	      /**< @brief Sample rate in Hz. */
	unsigned int channels;		      /**< @brief Number of interleaved channels. */
	enum ove_audio_sample_fmt sample_fmt; /**< @brief PCM sample format. */
};

/**
 * @brief Test two format descriptors for equality.
 *
 * @param[in] a  First format descriptor.
 * @param[in] b  Second format descriptor.
 * @return Non-zero if all fields match, zero otherwise.
 */
static inline int ove_audio_fmt_equal(const struct ove_audio_fmt *a, const struct ove_audio_fmt *b)
{
	return a->sample_rate == b->sample_rate && a->channels == b->channels &&
	       a->sample_fmt == b->sample_fmt;
}

/* ── Audio buffer ───────────────────────────────────────────────── */

/**
 * @brief Audio buffer passed between nodes during graph processing.
 *
 * Holds a pointer to interleaved PCM data, the number of frames
 * present, and a reference to the format that describes each sample.
 */
struct ove_audio_buf {
	void *data;			 /**< @brief Pointer to interleaved sample data. */
	unsigned int frames;		 /**< @brief Number of frames in @c data. */
	const struct ove_audio_fmt *fmt; /**< @brief Format descriptor for this buffer. */
};

/* ── Node vtable ────────────────────────────────────────────────── */

/**
 * @brief Virtual function table (vtable) for an audio processing node.
 *
 * Each node kind implements a subset of these callbacks.  NULL pointers
 * are treated as no-ops by the graph engine (except @c process, which
 * must be provided).
 */
struct ove_audio_node_ops {
	/**
     * @brief Negotiate format during graph build (topological order).
     *
     * Called once per node during ove_audio_graph_build().
     * - Sources: @p in_fmt is NULL; the node must fill @p out_fmt from
     *   its internal configuration.
     * - Processors: receive upstream @p in_fmt; must fill @p out_fmt.
     * - Sinks: receive upstream @p in_fmt; @p out_fmt is NULL; validate
     *   input and return an error if the format is unsupported.
     *
     * @param[in]  ctx      Node context pointer supplied at registration.
     * @param[in]  in_fmt   Upstream output format, or NULL for sources.
     * @param[out] out_fmt  Format this node will produce, or NULL for sinks.
     * @return 0 on success, negative error code on failure.
     */
	int (*configure)(void *ctx, const struct ove_audio_fmt *in_fmt,
			 struct ove_audio_fmt *out_fmt);

	/**
     * @brief Start the node (called on graph start).  NULL = no-op.
     *
     * @param[in] ctx  Node context pointer.
     * @return 0 on success, negative error code on failure.
     */
	int (*start)(void *ctx);

	/**
     * @brief Stop the node (called on graph stop).  NULL = no-op.
     *
     * @param[in] ctx  Node context pointer.
     * @return 0 on success, negative error code on failure.
     */
	int (*stop)(void *ctx);

	/**
     * @brief Process one buffer period.
     *
     * Called in topological order each graph cycle:
     * - Sources: @p in is NULL; the node must fill @p out.
     * - Processors: read from @p in, write to @p out (separate buffers).
     * - Sinks: read from @p in; @p out is NULL.
     *
     * @param[in]  ctx  Node context pointer.
     * @param[in]  in   Input buffer, or NULL for sources.
     * @param[out] out  Output buffer to fill, or NULL for sinks.
     * @return 0 on success, negative error code on failure.
     */
	int (*process)(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out);

	/**
     * @brief Release all resources owned by the node context.
     *
     * Called when the node is removed from the graph.  May be NULL.
     *
     * @param[in] ctx  Node context pointer.
     */
	void (*destroy)(void *ctx);
};

/* ── Node types ─────────────────────────────────────────────────── */

/**
 * @brief Role of a node within the audio graph.
 */
enum ove_audio_node_type {
	OVE_AUDIO_NODE_SOURCE,	  /**< @brief Produces audio; has no upstream connection. */
	OVE_AUDIO_NODE_PROCESSOR, /**< @brief Transforms audio; has one upstream connection. */
	OVE_AUDIO_NODE_SINK,	  /**< @brief Consumes audio; has no downstream connection. */
};

/**
 * @brief Descriptor for a single node in the audio graph.
 *
 * Populated by ove_audio_graph_add_node() and stored inside
 * @c ove_audio_graph::nodes[].
 */
struct ove_audio_node {
	const char *name;		      /**< @brief Human-readable node name. */
	enum ove_audio_node_type type;	      /**< @brief Source, processor, or sink. */
	const struct ove_audio_node_ops *ops; /**< @brief Vtable for this node. */
	void *ctx;		      /**< @brief Opaque context forwarded to every vtable call. */
	struct ove_audio_fmt out_fmt; /**< @brief Output format resolved during graph build. */
};

/* ── Built-in node factories ────────────────────────────────────── */

struct ove_audio_graph; /* forward declaration */

/**
 * @brief Channel routing table used by ove_audio_node_channel_map().
 *
 * Describes a remapping from any number of input channels to
 * @c out_channels output channels.  Each entry in @c map gives the
 * zero-based input channel index for the corresponding output channel;
 * a value of -1 produces silence on that output channel.
 */
struct ove_audio_channel_map {
	unsigned int out_channels;	 /**< @brief Number of output channels produced. */
	int map[OVE_AUDIO_MAX_CHANNELS]; /**< @brief map[out_ch] = in_ch index, or -1 for silence. */
};

/**
 * @brief Callback invoked by the tap node for every processed buffer.
 *
 * @param[in] buf        Buffer containing the observed audio data.
 * @param[in] user_data  Opaque pointer supplied at node creation.
 */
typedef void (*ove_audio_tap_fn)(const struct ove_audio_buf *buf, void *user_data);

/* The built-in factory functions below internally allocate a per-node
 * context with OVE_BACKEND_MALLOC.  Under CONFIG_OVE_ZERO_HEAP the gate
 * OVE_HEAP_AUDIO hides them: link fails rather than silently trapping,
 * nudging the application toward its own static-context processor nodes. */
#ifdef OVE_HEAP_AUDIO

/**
 * @brief Add a sample-format converter processor node to the graph.
 *
 * Inserts a processor that converts any upstream sample format to
 * @p target_fmt while preserving sample rate and channel count.
 *
 * @param[in] g           Graph to add the node to.
 * @param[in] target_fmt  Desired output sample format.
 * @param[in] name        Human-readable name for the node.
 * @return Non-negative node index on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO and @c OVE_HEAP_AUDIO.
 * @see ove_audio_graph_add_node, ove_audio_graph_connect
 */
int ove_audio_node_converter(struct ove_audio_graph *g, enum ove_audio_sample_fmt target_fmt,
			     const char *name);

/**
 * @brief Add a channel-mapping processor node to the graph.
 *
 * Inserts a processor that reorders, duplicates, or silences channels
 * according to @p map.  The output channel count equals
 * @c map->out_channels.
 *
 * @param[in] g     Graph to add the node to.
 * @param[in] map   Channel routing descriptor.
 * @param[in] name  Human-readable name for the node.
 * @return Non-negative node index on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO and @c OVE_HEAP_AUDIO.
 * @see ove_audio_node_converter
 */
int ove_audio_node_channel_map(struct ove_audio_graph *g, const struct ove_audio_channel_map *map,
			       const char *name);

/**
 * @brief Add a gain processor node to the graph.
 *
 * Inserts a processor that applies a fixed gain of @p gain_db decibels
 * to every sample.  Positive values amplify; negative values attenuate.
 * The output format is identical to the input format.
 *
 * @param[in] g        Graph to add the node to.
 * @param[in] gain_db  Gain in decibels (e.g. -6.0f for -6 dB).
 * @param[in] name     Human-readable name for the node.
 * @return Non-negative node index on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO and @c OVE_HEAP_AUDIO.
 */
int ove_audio_node_gain(struct ove_audio_graph *g, float gain_db, const char *name);

/**
 * @brief Add a tap (observer) sink node to the graph.
 *
 * Inserts a sink that calls @p fn for every audio buffer processed.
 * The callback receives a pointer to the upstream buffer; data must not
 * be stored beyond the duration of the callback.
 *
 * @param[in] g          Graph to add the node to.
 * @param[in] fn         Callback invoked each processing cycle.
 * @param[in] user_data  Opaque pointer forwarded to @p fn.
 * @param[in] name       Human-readable name for the node.
 * @return Non-negative node index on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO and @c OVE_HEAP_AUDIO.
 * @see ove_audio_node_gain
 */
int ove_audio_node_tap(struct ove_audio_graph *g, ove_audio_tap_fn fn, void *user_data,
		       const char *name);

#endif /* OVE_HEAP_AUDIO */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_audio_node group */

#endif /* OVE_AUDIO_NODE_H */
