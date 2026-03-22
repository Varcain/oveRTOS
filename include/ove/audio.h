/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_AUDIO_H
#define OVE_AUDIO_H

/**
 * @defgroup ove_audio Audio (I2S)
 * @brief I2S audio streaming with a process callback.
 *
 * Provides a portable audio I/O pipeline driven by a user-supplied
 * processing callback.  The backend spawns a real-time thread that
 * calls @c ove_audio_process_fn() once per period with interleaved
 * PCM input and output buffers.
 *
 * Typical usage:
 * @code
 *   static void my_process(int16_t *out, const int16_t *in,
 *                           unsigned int frames, void *ctx) {
 *       // process audio here
 *   }
 *
 *   struct ove_audio_config cfg = {
 *       .sample_rate       = 48000,
 *       .channels          = 2,
 *       .bit_depth         = 16,
 *       .frames_per_buffer = 256,
 *   };
 *   ove_audio_init(&cfg, my_process, NULL);
 *   ove_audio_start();
 * @endcode
 *
 * @note Requires @c CONFIG_OVE_AUDIO.  When the option is disabled every
 *       function is replaced by a stub that returns @c OVE_ERR_NOT_SUPPORTED.
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief User-supplied audio processing callback.
 *
 * Called once per buffer period from the audio thread.  The function
 * must complete within one buffer period to avoid underruns.
 *
 * @param[out] out         Output buffer (interleaved PCM, @p frame_count frames).
 * @param[in]  in          Input buffer (interleaved PCM, @p frame_count frames).
 *                         May be @c NULL if the backend has no input path.
 * @param[in]  frame_count Number of frames in @p out and @p in.
 * @param[in]  user_data   Opaque pointer supplied to ove_audio_init().
 */
typedef void (*ove_audio_process_fn)(int16_t *out, const int16_t *in,
					 unsigned int frame_count,
					 void *user_data);

/**
 * @brief Audio subsystem configuration.
 *
 * Passed to ove_audio_init() to configure the I2S peripheral and the
 * associated real-time processing thread.
 */
struct ove_audio_config {
	unsigned int sample_rate;        /**< Sample rate in Hz (e.g. 44100, 48000). */
	unsigned int channels;           /**< Number of interleaved channels (e.g. 2 for stereo). */
	unsigned int bit_depth;          /**< PCM bit depth per sample (e.g. 16, 24, 32). */
	unsigned int frames_per_buffer;  /**< Frames delivered to the callback per period. */
	/** RTOS thread priority for the audio thread; 0 means use the backend default. */
	unsigned int thread_priority;
	/** Stack size in bytes for the audio thread; 0 means use the backend default. */
	unsigned int thread_stack_size;
	unsigned int num_buffers;        /**< Number of DMA buffers; 0 means use the backend default. */
};

#ifdef CONFIG_OVE_AUDIO

/**
 * @brief Initialise the audio subsystem.
 *
 * Configures the I2S peripheral and spawns the audio processing thread.
 * Must be called before ove_audio_start().
 *
 * @param[in] cfg       Audio configuration parameters.
 * @param[in] fn        Processing callback invoked once per buffer period.
 * @param[in] user_data Opaque pointer forwarded to every @p fn invocation.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_audio_init(const struct ove_audio_config *cfg,
			ove_audio_process_fn fn, void *user_data);

/**
 * @brief Start audio streaming.
 *
 * Activates DMA transfers and begins calling the processing callback.
 * ove_audio_init() must have been called successfully beforehand.
 *
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_audio_start(void);

/**
 * @brief Stop audio streaming and release hardware resources.
 *
 * The processing callback will not be invoked after this call returns.
 * Call ove_audio_start() to resume.
 *
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_audio_stop(void);

/**
 * @brief Pause audio streaming without releasing hardware resources.
 *
 * Suspends DMA transfers; the I2S peripheral remains configured.
 * Resume with ove_audio_resume().
 *
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_audio_pause(void);

/**
 * @brief Resume a previously paused audio stream.
 *
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_audio_resume(void);

/**
 * @brief Deinitialise the audio subsystem and free all resources.
 *
 * After this call the audio subsystem must be re-initialised with
 * ove_audio_init() before it can be used again.
 */
void ove_audio_deinit(void);

#else /* !CONFIG_OVE_AUDIO */

static inline int ove_audio_init(const struct ove_audio_config *cfg, ove_audio_process_fn fn, void *user_data) { (void)cfg; (void)fn; (void)user_data; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_audio_start(void) { return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_audio_stop(void) { return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_audio_pause(void) { return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_audio_resume(void) { return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_audio_deinit(void) { }

#endif /* CONFIG_OVE_AUDIO */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_AUDIO_H */
