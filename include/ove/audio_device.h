/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
 * @defgroup ove_audio_device Audio device node factories
 * @ingroup ove_audio
 * @brief Hardware-backed source and sink node factories for the audio graph.
 *
 * Provides factory functions that register hardware audio devices
 * (I2S, PDM) directly as source or sink nodes inside an
 * @ref ove_audio_graph.  Each factory allocates and configures the
 * necessary backend driver state and adds the node in one call.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @{
 */

#ifndef OVE_AUDIO_DEVICE_H
#define OVE_AUDIO_DEVICE_H

#include "ove/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Transport types ────────────────────────────────────────────── */

/**
 * @brief Audio hardware transport type.
 *
 * Selects the low-level audio bus or host API used by a device node.
 */
enum ove_audio_transport {
	OVE_AUDIO_TRANSPORT_I2S, /**< @brief I2S / TDM serial bus (e.g. codec, DAC, ADC). */
	OVE_AUDIO_TRANSPORT_PDM, /**< @brief PDM microphone interface. */
};

/* ── Device configuration ───────────────────────────────────────── */

/**
 * @brief Configuration descriptor for a hardware audio device node.
 *
 * Passed to ove_audio_device_source() or ove_audio_device_sink() to
 * describe the transport, stream format, and transport-specific
 * parameters.  Fields set to zero select the backend default.
 */
struct ove_audio_device_cfg {
	enum ove_audio_transport transport; /**< @brief Hardware transport selection. */
	struct ove_audio_fmt fmt;	    /**< @brief Desired audio stream format. */
	unsigned int num_buffers;	    /**< @brief DMA buffer count; 0 = backend default. */
	unsigned int thread_priority; /**< @brief Driver thread priority; 0 = backend default. */
	unsigned int
		thread_stack_size; /**< @brief Driver thread stack size in bytes; 0 = backend default. */

	union {
		/** @brief Parameters specific to I2S / TDM transport. */
		struct {
			unsigned int
				input_device; /**< @brief Input device selector (e.g. line-in, DMIC index). */
			unsigned int slot_mask; /**< @brief TDM slot bitmask; 0 = all slots. */
		} i2s;
		/** @brief Parameters specific to PDM microphone transport. */
		struct {
			unsigned int decimation; /**< @brief PDM decimation factor. */
			unsigned int clock_freq; /**< @brief PDM clock frequency in Hz. */
		} pdm;
	};
};

/* ── Device node factories ──────────────────────────────────────── */

#ifdef CONFIG_OVE_AUDIO

/**
 * @brief Add a hardware audio source node to the graph.
 *
 * Creates a source node backed by the hardware device described in
 * @p cfg and registers it with the graph.  The node captures audio
 * from the selected transport and exposes it as graph output each cycle.
 *
 * @param[in] g     Graph instance in the @c OVE_AUDIO_GRAPH_IDLE state.
 * @param[in] cfg   Device configuration describing the transport and format.
 * @param[in] name  Human-readable node name for diagnostics.
 * @return Non-negative node index on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_device_sink, ove_audio_graph_connect
 */
int ove_audio_device_source(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			    const char *name);

/**
 * @brief Add a hardware audio sink node to the graph.
 *
 * Creates a sink node backed by the hardware device described in
 * @p cfg and registers it with the graph.  The node consumes audio
 * from its upstream connection and delivers it to the selected transport
 * each processing cycle.
 *
 * @param[in] g     Graph instance in the @c OVE_AUDIO_GRAPH_IDLE state.
 * @param[in] cfg   Device configuration describing the transport and format.
 * @param[in] name  Human-readable node name for diagnostics.
 * @return Non-negative node index on success, negative error code on failure.
 *
 * @note Requires @c CONFIG_OVE_AUDIO.
 * @see ove_audio_device_source, ove_audio_graph_connect
 */
int ove_audio_device_sink(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			  const char *name);

#endif /* CONFIG_OVE_AUDIO */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_audio_device group */

#endif /* OVE_AUDIO_DEVICE_H */
