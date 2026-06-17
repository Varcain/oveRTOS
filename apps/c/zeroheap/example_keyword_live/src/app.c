/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Live DMIC Keyword Detection
 *
 * Real-time "yes"/"no" keyword detection using the on-board digital
 * MEMS microphones and the TFLM micro_speech model.
 *
 * Architecture:
 *   Audio callback (ISR priority)
 *     → writes PCM samples to lock-free ring buffer
 *   Inference thread (normal priority)
 *     → every ~1 second, snapshots the last second of audio
 *     → runs audio preprocessor model (49 × 30ms spectral features)
 *     → runs keyword classifier model (silence/unknown/yes/no)
 *     → logs detections
 */

#include "ove/ove.h"
#include "ove/infer.h"

#include <stdatomic.h>
#include <string.h>

#include "keyword_models.h"
#include "ring_buffer.h"

/* ── Model parameters ──────────────────────────────────────────────── */
#define FEATURE_SIZE 40
#define FEATURE_COUNT 49
#define FEATURE_ELEMENT_COUNT (FEATURE_SIZE * FEATURE_COUNT)
#define FEATURE_STRIDE_MS 20
#define FEATURE_DURATION_MS 30
#define AUDIO_SAMPLE_FREQ 16000
#define AUDIO_STRIDE_SAMPLES (FEATURE_STRIDE_MS * AUDIO_SAMPLE_FREQ / 1000)
#define AUDIO_DURATION_SAMPLES (FEATURE_DURATION_MS * AUDIO_SAMPLE_FREQ / 1000)
#define CATEGORY_COUNT 4
#define ARENA_SIZE 32768
#define CONFIDENCE_THRESHOLD 0.6f

static const char *labels[CATEGORY_COUNT] = {"silence", "unknown", "yes", "no"};

/* ── Shared static memory ──────────────────────────────────────────── */
static ring_buffer_t audio_ring;
static int8_t features[FEATURE_COUNT][FEATURE_SIZE];
static uint8_t __attribute__((aligned(16))) arena[ARENA_SIZE];
static ove_model_storage_t model_storage;

/* ── Audio callback ────────────────────────────────────────────────── */

/* Cross-thread counters: the audio callback writes them; the inference
 * thread reads `samples_written` for rate estimation.  C11 atomics (relaxed)
 * make the read-modify-write well-defined — a bare `volatile ++` shared
 * across threads is a data race. */
static atomic_uint audio_cb_count;
static atomic_uint samples_written;
/* ── DMIC processor node ───────────────────────────────────────────
 *
 * Receives clean mono PCM from the audio source (driver handles
 * I2S slot extraction) and:
 *   1. Feeds samples into the ring buffer for inference
 *   2. Passes audio through to the sink for monitoring
 */

static int dmic_proc_configure(void *ctx, const struct ove_audio_fmt *in_fmt,
			       struct ove_audio_fmt *out_fmt)
{
	(void)ctx;
	*out_fmt = *in_fmt;
	return OVE_OK;
}

static int dmic_proc_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	(void)ctx;
	const int16_t *src = (const int16_t *)in->data;
	int16_t *dst = (int16_t *)out->data;
	unsigned int frames = in->frames;
	unsigned int ch = in->fmt->channels;

	atomic_fetch_add_explicit(&audio_cb_count, 1, memory_order_relaxed);

	for (unsigned int f = 0; f < frames; f++) {
		int16_t sample = src[f * ch]; /* left / mono channel */

		/* Feed to ring buffer for inference */
		ring_buffer_write(&audio_ring, &sample, 1);
		atomic_fetch_add_explicit(&samples_written, 1, memory_order_relaxed);

		/* Passthrough to output for monitoring */
		for (unsigned int c = 0; c < ch; c++)
			dst[f * ch + c] = src[f * ch + c];
	}

	return OVE_OK;
}

static const struct ove_audio_node_ops dmic_proc_ops = {
	.configure = dmic_proc_configure,
	.process = dmic_proc_process,
};

/* ── Feature extraction ────────────────────────────────────────────── */

/*
 * Generate features from audio that may be at a different sample rate
 * than the model expects (16kHz).  If actual_rate != 16000, resample
 * each window on the fly using linear interpolation.
 */
static unsigned int g_actual_rate = AUDIO_SAMPLE_FREQ;
static int32_t g_dc_offset;
static int32_t g_gain = 1; /* adaptive gain, computed per window */

/* Noise gate: peak must exceed this (after DC removal) to be considered speech.
 * Below this, feed silence to the model. */
#define NOISE_GATE_THRESHOLD 500
#define TARGET_PEAK 15000

static int generate_features(const int16_t *audio_data, unsigned int audio_len)
{
	struct ove_model_config cfg = {
		.model_data = g_audio_preprocessor_int8_model_data,
		.model_size = g_audio_preprocessor_int8_model_data_size,
		.arena_size = ARENA_SIZE,
	};

	ove_model_t preproc;
	int rc = ove_model_init(&preproc, &model_storage, arena, &cfg);
	if (rc != OVE_OK)
		return rc;

	struct ove_tensor_info input_info, output_info;
	ove_model_input(preproc, 0, &input_info);
	ove_model_output(preproc, 0, &output_info);

	/*
	 * Scale stride and window size to the actual sample rate.
	 * Model expects: 480 samples (30ms) window, 320 samples (20ms) stride at 16kHz.
	 * At actual_rate: window = 30ms * actual_rate / 1000, stride = 20ms * actual_rate / 1000.
	 */
	unsigned int actual_window = FEATURE_DURATION_MS * g_actual_rate / 1000;
	unsigned int actual_stride = FEATURE_STRIDE_MS * g_actual_rate / 1000;

	unsigned int frame = 0;
	unsigned int offset = 0;
	while (offset + actual_window <= audio_len && frame < FEATURE_COUNT) {
		int16_t *input = (int16_t *)input_info.data;

		/* Resample from actual_rate to 16kHz, remove DC offset,
		 * and apply adaptive gain normalization. */
		for (unsigned int i = 0; i < AUDIO_DURATION_SAMPLES; i++) {
			unsigned int src_idx = offset + (i * g_actual_rate / AUDIO_SAMPLE_FREQ);
			int32_t sample;
			if (src_idx < audio_len)
				sample = (int32_t)audio_data[src_idx];
			else
				sample = 0;

			/* Remove DC offset */
			sample -= g_dc_offset;

			/* Adaptive gain: normalize so window peak → ~20000.
			 * Only apply when signal is above noise gate. */
			sample = (int32_t)(sample * g_gain);

			/* Clamp to int16 range */
			if (sample > 32767)
				sample = 32767;
			if (sample < -32768)
				sample = -32768;

			input[i] = (int16_t)sample;
		}

		rc = ove_model_invoke(preproc);
		if (rc != OVE_OK) {
			ove_model_deinit(preproc);
			return rc;
		}

		int8_t *output = (int8_t *)output_info.data;
		memcpy(features[frame], output, FEATURE_SIZE);

		frame++;
		offset += actual_stride;
	}

	ove_model_deinit(preproc);
	return OVE_OK;
}

/* ── Classification ────────────────────────────────────────────────── */

static int classify_keyword(int *prediction_idx, float *confidence)
{
	struct ove_model_config cfg = {
		.model_data = g_micro_speech_quantized_model_data,
		.model_size = g_micro_speech_quantized_model_data_size,
		.arena_size = ARENA_SIZE,
	};

	ove_model_t classifier;
	int rc = ove_model_init(&classifier, &model_storage, arena, &cfg);
	if (rc != OVE_OK)
		return rc;

	struct ove_tensor_info input_info, output_info;
	ove_model_input(classifier, 0, &input_info);
	ove_model_output(classifier, 0, &output_info);

	int8_t *input = (int8_t *)input_info.data;
	memcpy(input, features, FEATURE_ELEMENT_COUNT);

	rc = ove_model_invoke(classifier);
	if (rc != OVE_OK) {
		ove_model_deinit(classifier);
		return rc;
	}

	int8_t *scores = (int8_t *)output_info.data;
	int best = 0;
	for (int i = 1; i < CATEGORY_COUNT; i++) {
		if (scores[i] > scores[best])
			best = i;
	}

	*prediction_idx = best;
	*confidence = ((float)scores[best] + 128.0f) / 255.0f;

	ove_model_deinit(classifier);
	return OVE_OK;
}

/* ── Inference thread ──────────────────────────────────────────────── */

static int16_t audio_window[AUDIO_SAMPLE_FREQ]; /* 1 second */

static void infer_thread(void *arg)
{
	(void)arg;

	OVE_LOG_INF("Inference thread started — listening...");

	/* Initial wait: let ring buffer fill and measure actual sample rate */
	ove_thread_sleep_ms(2000);
	uint32_t prev_samples = atomic_load_explicit(&samples_written, memory_order_relaxed);

	for (;;) {
		ove_thread_sleep_ms(1000);

		/* Measure actual sample rate */
		uint32_t cur_samples = atomic_load_explicit(&samples_written, memory_order_relaxed);
		uint32_t actual_rate = cur_samples - prev_samples;
		prev_samples = cur_samples;

		/*
		 * The model needs exactly 16000 samples (1 second at 16kHz).
		 * If actual rate differs, read that many samples instead —
		 * they represent 1 second of real time regardless of rate.
		 */
		unsigned int read_count = (actual_rate > 0) ? actual_rate : AUDIO_SAMPLE_FREQ;
		if (read_count > AUDIO_SAMPLE_FREQ)
			read_count = AUDIO_SAMPLE_FREQ; /* clamp to buffer size */

		unsigned int avail = ring_buffer_available(&audio_ring);
		if (avail < read_count) {
			OVE_LOG_WRN("Ring buffer: only %u samples (need %u)", avail, read_count);
			continue;
		}

		/* Snapshot last ~1 second of audio */
		ring_buffer_read_last(&audio_ring, audio_window, read_count);

		/* Audio diagnostics */
		int16_t peak = 0;
		for (unsigned int i = 0; i < read_count; i++) {
			int16_t s = audio_window[i] < 0 ? -audio_window[i] : audio_window[i];
			if (s > peak)
				peak = s;
		}

		OVE_LOG_INF("Audio: peak=%d, rate=%u, read=%u", peak, actual_rate, read_count);

		/* Update actual sample rate for feature extraction resampling */
		g_actual_rate = actual_rate > 0 ? actual_rate : AUDIO_SAMPLE_FREQ;

		if (peak < 10) {
			OVE_LOG_WRN("Audio silent — check DMIC");
			continue;
		}

		/* Compute DC offset from this window */
		{
			int64_t sum = 0;
			for (unsigned int i = 0; i < read_count; i++)
				sum += audio_window[i];
			g_dc_offset = (int32_t)(sum / (int64_t)read_count);
		}

		/* Compute peak after DC removal for noise gate + adaptive gain */
		int32_t dc_peak = 0;
		for (unsigned int i = 0; i < read_count; i++) {
			int32_t s = (int32_t)audio_window[i] - g_dc_offset;
			if (s < 0)
				s = -s;
			if (s > dc_peak)
				dc_peak = s;
		}

		if (dc_peak < NOISE_GATE_THRESHOLD) {
			/* Below noise floor — skip inference */
			continue;
		}

		/* Adaptive gain: scale peak to ~TARGET_PEAK */
		g_gain = TARGET_PEAK / dc_peak;
		if (g_gain < 1)
			g_gain = 1;
		if (g_gain > 200)
			g_gain = 200;

		OVE_LOG_INF("  dc_peak=%d, gain=%d", (int)dc_peak, (int)g_gain);

		/* Stage 1: Audio → spectral features */
		int rc = generate_features(audio_window, read_count);
		if (rc != OVE_OK) {
			OVE_LOG_ERR("Feature extraction failed: %d", rc);
			continue;
		}

		/* Stage 2: Features → keyword classification */
		int prediction;
		float confidence;
		rc = classify_keyword(&prediction, &confidence);
		if (rc != OVE_OK) {
			OVE_LOG_ERR("Classification failed: %d", rc);
			continue;
		}

		/* Log detections (skip silence/unknown at low confidence) */
		if (prediction > 1 && confidence > CONFIDENCE_THRESHOLD) {
			OVE_LOG_INF(">>> Keyword: \"%s\" (%.0f%%)", labels[prediction],
				    (double)(confidence * 100.0f));
		}
	}
}

/* ── Entry point ───────────────────────────────────────────────────── */

OVE_THREAD_DEFINE_STATIC(infer_thread_handle, 8192, infer_thread, NULL, OVE_PRIO_NORMAL, "infer");

void ove_main(void)
{
	OVE_LOG_INF("=== Live DMIC Keyword Detection ===");
	OVE_LOG_INF("Models: preprocessor %u + classifier %u bytes",
		    g_audio_preprocessor_int8_model_data_size,
		    g_micro_speech_quantized_model_data_size);

	/* Initialize ring buffer */
	ring_buffer_init(&audio_ring);

	/* Initialize audio graph: I2S DMIC → processor → I2S headphone */
	static struct ove_audio_graph audio_graph;
	struct ove_audio_device_cfg dev_cfg = {
		.transport = OVE_AUDIO_TRANSPORT_I2S,
		.fmt =
			{
				.sample_rate = 16000,
				.channels = 1,
				.sample_fmt = OVE_AUDIO_FMT_S16,
			},
		.i2s.input_device = 1, /* DMIC */
	};

	int rc = ove_audio_graph_init(&audio_graph, 512); /* 32ms at 16kHz */
	if (rc != OVE_OK) {
		OVE_LOG_ERR("Audio graph init failed: %d", rc);
		ove_run();
		return;
	}

	int src = ove_audio_device_source(&audio_graph, &dev_cfg, "dmic-in");
	int proc = ove_audio_graph_add_node(&audio_graph, &dmic_proc_ops, NULL, "dmic-proc",
					    OVE_AUDIO_NODE_PROCESSOR);
	int sink = ove_audio_device_sink(&audio_graph, &dev_cfg, "hp-out");

	if (src < 0 || proc < 0 || sink < 0) {
		OVE_LOG_ERR("Audio node creation failed: %d %d %d", src, proc, sink);
		ove_run();
		return;
	}

	ove_audio_graph_connect(&audio_graph, src, proc);
	ove_audio_graph_connect(&audio_graph, proc, sink);

	rc = ove_audio_graph_build(&audio_graph);
	if (rc != OVE_OK) {
		OVE_LOG_ERR("Audio graph build failed: %d", rc);
		ove_run();
		return;
	}

	rc = ove_audio_graph_start(&audio_graph);
	if (rc != OVE_OK) {
		OVE_LOG_ERR("Audio graph start failed: %d", rc);
		ove_run();
		return;
	}
	OVE_LOG_INF("Audio streaming: 16kHz mono, DMIC input");

	/* infer_thread_handle is statically allocated; the thread starts
	 * once ove_run() engages the scheduler. */

	OVE_LOG_INF("Say \"yes\" or \"no\" near the microphone...");

	ove_run();
}
