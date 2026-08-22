/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Live DMIC Keyword Detection — C++ implementation
 *
 * Uses the C++ audio graph template wrapper (add_processor<T>) and the
 * RAII Model<ArenaSize> class.  No raw function pointers or void* in
 * application code.
 */

#include <ove/ove.hpp>
#include <ove/audio.hpp>
#include <ove/infer.hpp>
#include <ove/thread.hpp>

#include <cstring>
#include <atomic>

#include "keyword_models.h"

// ── Constants ──────────────────────────────────────────────────────────

static constexpr unsigned FEATURE_SIZE = 40;
static constexpr unsigned FEATURE_COUNT = 49;
static constexpr unsigned FEATURE_ELEMENTS = FEATURE_SIZE * FEATURE_COUNT;
static constexpr unsigned AUDIO_SAMPLE_FREQ = 16000;
static constexpr unsigned FEATURE_STRIDE_MS = 20;
static constexpr unsigned FEATURE_DURATION_MS = 30;
static constexpr unsigned AUDIO_STRIDE_SAMPLES = FEATURE_STRIDE_MS * AUDIO_SAMPLE_FREQ / 1000;
static constexpr unsigned AUDIO_DURATION_SAMPLES = FEATURE_DURATION_MS * AUDIO_SAMPLE_FREQ / 1000;
static constexpr unsigned CATEGORY_COUNT = 4;
static constexpr unsigned ARENA_SIZE = 32768;
static constexpr float CONFIDENCE_THRESHOLD = 0.6f;
static constexpr int NOISE_GATE_THRESHOLD = 500;
static constexpr int TARGET_PEAK = 15000;

static const char *labels[CATEGORY_COUNT] = {"silence", "unknown", "yes", "no"};

// ── Lock-free ring buffer ──────────────────────────────────────────────

static constexpr unsigned RING_BUF_CAPACITY = 32768;
static constexpr unsigned RING_BUF_MASK = RING_BUF_CAPACITY - 1;

struct RingBuffer {
	int16_t data[RING_BUF_CAPACITY]{};
	std::atomic<unsigned> head{0};
	std::atomic<unsigned> tail{0};

	void write(int16_t sample)
	{
		data[head.load(std::memory_order_relaxed) & RING_BUF_MASK] = sample;
		head.fetch_add(1, std::memory_order_release);
	}

	unsigned available() const
	{
		return head.load(std::memory_order_acquire) - tail.load(std::memory_order_relaxed);
	}

	void read_last(int16_t *out, unsigned count)
	{
		unsigned h = head.load(std::memory_order_acquire);
		unsigned start = (h >= count) ? h - count : 0;
		for (unsigned i = 0; i < count; i++)
			out[i] = data[(start + i) & RING_BUF_MASK];
		tail.store(h, std::memory_order_relaxed);
	}
};

// ── Shared state ───────────────────────────────────────────────────────

static RingBuffer audio_ring;
static int8_t features[FEATURE_COUNT][FEATURE_SIZE];
/* Written by the audio callback, read by the inference thread.  Relaxed
 * suffices: it is a monotonic counter used only for rate estimation; the
 * audio samples themselves are published through `audio_ring`'s own
 * acquire/release ordering. */
static std::atomic<uint32_t> samples_written{0};
static unsigned g_actual_rate = AUDIO_SAMPLE_FREQ;
static int32_t g_dc_offset;
static int32_t g_gain = 1;

// ── DMIC processor node ────────────────────────────────────────────────
// Uses the C++ add_processor<T> template — no raw vtable needed.

struct DmicProcessor {
	int process(const struct ove_audio_buf *in, struct ove_audio_buf *out)
	{
		const int16_t *src = static_cast<const int16_t *>(in->data);
		int16_t *dst = static_cast<int16_t *>(out->data);
		unsigned frames = in->frames;
		unsigned ch = in->fmt->channels;

		for (unsigned f = 0; f < frames; f++) {
			int16_t sample = src[f * ch];
			audio_ring.write(sample);
			samples_written.fetch_add(1, std::memory_order_relaxed);
			for (unsigned c = 0; c < ch; c++)
				dst[f * ch + c] = src[f * ch + c];
		}
		return OVE_OK;
	}
};

// ── Feature extraction ─────────────────────────────────────────────────

static int generate_features(const int16_t *audio, unsigned len)
{
	ove::Model<ARENA_SIZE> preproc({
		g_audio_preprocessor_int8_model_data,
		g_audio_preprocessor_int8_model_data_size,
		ARENA_SIZE,
	});

	unsigned actual_window = FEATURE_DURATION_MS * g_actual_rate / 1000;
	unsigned actual_stride = FEATURE_STRIDE_MS * g_actual_rate / 1000;

	unsigned frame = 0, offset = 0;
	while (offset + actual_window <= len && frame < FEATURE_COUNT) {
		auto *input = preproc.input_data<int16_t>(0);

		for (unsigned i = 0; i < AUDIO_DURATION_SAMPLES; i++) {
			unsigned src_idx = offset + (i * g_actual_rate / AUDIO_SAMPLE_FREQ);
			int32_t s = (src_idx < len) ? audio[src_idx] : 0;
			s -= g_dc_offset;
			s *= g_gain;
			if (s > 32767)
				s = 32767;
			if (s < -32768)
				s = -32768;
			input[i] = static_cast<int16_t>(s);
		}

		if (auto r = preproc.invoke(); !r)
			return static_cast<int>(r.error());

		std::memcpy(features[frame], preproc.output_data<int8_t>(0), FEATURE_SIZE);
		frame++;
		offset += actual_stride;
	}
	return OVE_OK;
}

// ── Classification ─────────────────────────────────────────────────────

static int classify_keyword(int &prediction, float &confidence)
{
	ove::Model<ARENA_SIZE> classifier({
		g_micro_speech_quantized_model_data,
		g_micro_speech_quantized_model_data_size,
		ARENA_SIZE,
	});

	std::memcpy(classifier.input_data<int8_t>(0), features, FEATURE_ELEMENTS);

	if (auto r = classifier.invoke(); !r)
		return static_cast<int>(r.error());

	auto *scores = classifier.output_data<int8_t>(0);
	unsigned int best = 0;
	for (unsigned int i = 1; i < CATEGORY_COUNT; i++)
		if (scores[i] > scores[best])
			best = i;

	prediction = static_cast<int>(best);
	confidence = (static_cast<float>(scores[best]) + 128.0f) / 255.0f;
	return OVE_OK;
}

// ── Inference thread ───────────────────────────────────────────────────

static int16_t audio_window[AUDIO_SAMPLE_FREQ];

static void infer_thread(void *)
{
	OVE_LOG_INF("Inference thread started — listening...");
	ove_thread_sleep_ms(2000);
	uint32_t prev_samples = samples_written.load(std::memory_order_relaxed);

	for (;;) {
		ove_thread_sleep_ms(1000);

		uint32_t cur = samples_written.load(std::memory_order_relaxed);
		uint32_t actual_rate = cur - prev_samples;
		prev_samples = cur;

		unsigned read_count = actual_rate > 0 ? actual_rate : AUDIO_SAMPLE_FREQ;
		if (read_count > AUDIO_SAMPLE_FREQ)
			read_count = AUDIO_SAMPLE_FREQ;

		if (audio_ring.available() < read_count)
			continue;
		audio_ring.read_last(audio_window, read_count);

		// Audio stats
		int16_t peak = 0;
		for (unsigned i = 0; i < read_count; i++) {
			int16_t s = audio_window[i] < 0 ? -audio_window[i] : audio_window[i];
			if (s > peak)
				peak = s;
		}
		OVE_LOG_INF("Audio: peak=%d, rate=%u, read=%u", peak,
			    static_cast<unsigned int>(actual_rate), read_count);

		g_actual_rate = actual_rate > 0 ? actual_rate : AUDIO_SAMPLE_FREQ;
		if (peak < 10) {
			OVE_LOG_WRN("Audio silent");
			continue;
		}

		// DC offset
		int64_t sum = 0;
		for (unsigned i = 0; i < read_count; i++)
			sum += audio_window[i];
		g_dc_offset = static_cast<int32_t>(sum / static_cast<int64_t>(read_count));

		// Noise gate + adaptive gain
		int32_t dc_peak = 0;
		for (unsigned i = 0; i < read_count; i++) {
			int32_t s = audio_window[i] - g_dc_offset;
			if (s < 0)
				s = -s;
			if (s > dc_peak)
				dc_peak = s;
		}
		if (dc_peak < NOISE_GATE_THRESHOLD)
			continue;

		g_gain = TARGET_PEAK / dc_peak;
		if (g_gain < 1)
			g_gain = 1;
		if (g_gain > 200)
			g_gain = 200;
		OVE_LOG_INF("  dc_peak=%d, gain=%d", (int)dc_peak, (int)g_gain);

		// Inference pipeline
		int rc = generate_features(audio_window, read_count);
		if (rc != OVE_OK) {
			OVE_LOG_ERR("Features failed: %d", rc);
			continue;
		}

		int prediction;
		float confidence;
		rc = classify_keyword(prediction, confidence);
		if (rc != OVE_OK) {
			OVE_LOG_ERR("Classify failed: %d", rc);
			continue;
		}

		if (prediction > 1 && confidence > CONFIDENCE_THRESHOLD) {
			OVE_LOG_INF(">>> Keyword: \"%s\" (%.0f%%)", labels[prediction],
				    static_cast<double>(confidence * 100.0f));
		}
	}
}

// ── Entry point ────────────────────────────────────────────────────────

OVE_MAIN()
{
	OVE_LOG_INF("=== Live DMIC Keyword Detection (C++) ===");
	OVE_LOG_INF("Models: preprocessor %u + classifier %u bytes",
		    g_audio_preprocessor_int8_model_data_size,
		    g_micro_speech_quantized_model_data_size);

	// Audio graph: DMIC → processor → headphone
	static ove::audio::Graph graph;
	struct ove_audio_device_cfg dev_cfg = {};
	dev_cfg.transport = OVE_AUDIO_TRANSPORT_I2S;
	dev_cfg.fmt.sample_rate = 16000;
	dev_cfg.fmt.channels = 1;
	dev_cfg.fmt.sample_fmt = OVE_AUDIO_FMT_S16;
	dev_cfg.i2s.input_device = 1;

	(void)graph.init(512);

	auto src_r = graph.device_source(&dev_cfg, "dmic-in");
	static DmicProcessor dmic_proc;
	auto proc_r = graph.add_processor(dmic_proc, "dmic-proc");
	auto sink_r = graph.device_sink(&dev_cfg, "hp-out");

	if (!src_r || !proc_r || !sink_r) {
		OVE_LOG_ERR("Audio node creation failed");
		ove::run();
		return;
	}

	(void)graph.connect(*src_r, *proc_r);
	(void)graph.connect(*proc_r, *sink_r);
	(void)graph.build();
	(void)graph.start();
	OVE_LOG_INF("Audio streaming: 16kHz mono, DMIC input");

	// Inference thread — zero-heap mode: wrapper carries kernel storage
	// and stack inline as struct members; static function-scope keeps
	// it pinned for the program lifetime.
	static ove::Thread<8192> infer_t(infer_thread, nullptr, OVE_PRIO_NORMAL, "infer");
	(void)infer_t;

	OVE_LOG_INF("Say \"yes\" or \"no\" near the microphone...");
	ove::run();
}
