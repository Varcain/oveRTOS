// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Live DMIC Keyword Detection — Rust implementation
//!
//! Zero `unsafe` in application code.  All FFI bridging is inside
//! the `ove` crate's binding layer.

#![cfg_attr(not(feature = "std"), no_std)]

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};
use ove::{Error, Thread};

ove::main!(app_main);

// ── Constants ──────────────────────────────────────────────────────────

const FEATURE_SIZE: usize = 40;
const FEATURE_COUNT: usize = 49;
const AUDIO_SAMPLE_FREQ: u32 = 16000;
const FEATURE_STRIDE_MS: u32 = 20;
const FEATURE_DURATION_MS: u32 = 30;
const AUDIO_DURATION_SAMPLES: usize = (FEATURE_DURATION_MS * AUDIO_SAMPLE_FREQ / 1000) as usize;
const CATEGORY_COUNT: usize = 4;
const ARENA_SIZE: usize = 32768;
const CONFIDENCE_THRESHOLD: f32 = 0.6;
const NOISE_GATE_THRESHOLD: i32 = 500;
const TARGET_PEAK: i32 = 15000;
const RING_CAPACITY: usize = 32768;
const RING_MASK: usize = RING_CAPACITY - 1;
// I2S slot handling is now done by the driver — app sees clean PCM.

static LABELS: [&str; CATEGORY_COUNT] = ["silence", "unknown", "yes", "no"];

// ── Model data (linked from C objects) ─────────────────────────────────

ove::model_data!(preprocessor_model,
    g_audio_preprocessor_int8_model_data,
    g_audio_preprocessor_int8_model_data_len);

ove::model_data!(classifier_model,
    g_micro_speech_quantized_model_data,
    g_micro_speech_quantized_model_data_len);

// ── Lock-free SPSC ring buffer ────────────────────────────────────────
//
// Interior-mutable: `write()` takes `&self` so the buffer can live in
// a plain `static` (no `static mut`).  Safety of the data cell relies
// on the SPSC invariant: exactly one writer (audio ISR) and one reader
// (inference thread), synchronized via atomic head/tail indices.

struct RingBuffer {
    data: UnsafeCell<[i16; RING_CAPACITY]>,
    head: AtomicU32,
    tail: AtomicU32,
}

// SAFETY: SPSC — single producer (audio callback), single consumer
// (inference thread).  The atomic indices provide the synchronization.
unsafe impl Sync for RingBuffer {}

impl RingBuffer {
    const fn new() -> Self {
        Self {
            data: UnsafeCell::new([0; RING_CAPACITY]),
            head: AtomicU32::new(0),
            tail: AtomicU32::new(0),
        }
    }

    /// Write a sample (single-producer side — audio callback).
    fn write(&self, sample: i16) {
        let h = self.head.load(Ordering::Relaxed) as usize;
        // SAFETY: Only one writer (audio callback) ever calls this.
        unsafe { (*self.data.get())[h & RING_MASK] = sample };
        self.head.fetch_add(1, Ordering::Release);
    }

    fn available(&self) -> u32 {
        self.head.load(Ordering::Acquire) - self.tail.load(Ordering::Relaxed)
    }

    /// Read the most recent samples into `out` (single-consumer side).
    fn read_last(&self, out: &mut [i16]) {
        let count = out.len();
        let h = self.head.load(Ordering::Acquire) as usize;
        let start = if h >= count { h - count } else { 0 };
        for i in 0..count {
            // SAFETY: Only one reader (inference thread) calls this.
            out[i] = unsafe { (*self.data.get())[(start + i) & RING_MASK] };
        }
        self.tail.store(h as u32, Ordering::Relaxed);
    }
}

// ── Shared state ───────────────────────────────────────────────────────

static AUDIO_RING: RingBuffer = RingBuffer::new();
static SAMPLES_WRITTEN: AtomicU32 = AtomicU32::new(0);

// ── DMIC processor node ────────────────────────────────────────────────

struct DmicProcessor;

impl ove::audio::AudioProcessor for DmicProcessor {
    fn process(&mut self, input: &ove::audio::AudioBuf, output: &ove::audio::AudioBuf) {
        let src = input.data_s16();
        let dst = output.data_s16_mut();
        let frames = input.frames() as usize;
        let ch = input.channels() as usize;

        for f in 0..frames {
            let sample = src[f * ch]; // left / mono
            AUDIO_RING.write(sample);
            SAMPLES_WRITTEN.fetch_add(1, Ordering::Relaxed);

            // Passthrough to output for monitoring
            for c in 0..ch {
                dst[f * ch + c] = src[f * ch + c];
            }
        }
    }
}

// ── DSP parameters ────────────────────────────────────────────────────

struct DspState {
    actual_rate: u32,
    dc_offset: i32,
    gain: i32,
}

// ── Feature extraction ─────────────────────────────────────────────────

fn generate_features(
    audio: &[i16],
    features: &mut [[i8; FEATURE_SIZE]; FEATURE_COUNT],
    storage: &mut ove::infer::ModelStorage<ARENA_SIZE>,
    dsp: &DspState,
) -> ove::Result<()> {
    let model = storage.load(preprocessor_model())?;

    let actual_window = (FEATURE_DURATION_MS * dsp.actual_rate / 1000) as usize;
    let actual_stride = (FEATURE_STRIDE_MS * dsp.actual_rate / 1000) as usize;

    let mut frame = 0usize;
    let mut offset = 0usize;
    while offset + actual_window <= audio.len() && frame < FEATURE_COUNT {
        let input = model.input_slice_mut::<i16>(0)?;

        for i in 0..AUDIO_DURATION_SAMPLES {
            let src_idx = offset + (i * dsp.actual_rate as usize / AUDIO_SAMPLE_FREQ as usize);
            let raw = if src_idx < audio.len() { audio[src_idx] as i32 } else { 0 };
            let s = ((raw - dsp.dc_offset) * dsp.gain).clamp(-32768, 32767);
            input[i] = s as i16;
        }

        model.invoke()?;

        let output = model.output_slice::<i8>(0)?;
        features[frame].copy_from_slice(&output[..FEATURE_SIZE]);

        frame += 1;
        offset += actual_stride;
    }

    Ok(())
}

// ── Classification ─────────────────────────────────────────────────────

struct Prediction {
    label: usize,
    confidence: f32,
}

fn classify_keyword(
    features: &[[i8; FEATURE_SIZE]; FEATURE_COUNT],
    storage: &mut ove::infer::ModelStorage<ARENA_SIZE>,
) -> ove::Result<Prediction> {
    let model = storage.load(classifier_model())?;

    // Flatten 2D features into tensor input
    let input = model.input_slice_mut::<i8>(0)?;
    for (i, row) in features.iter().enumerate() {
        input[i * FEATURE_SIZE..(i + 1) * FEATURE_SIZE].copy_from_slice(row);
    }

    model.invoke()?;

    let scores = model.output_slice::<i8>(0)?;
    let best = (1..CATEGORY_COUNT).fold(0, |best, i| {
        if scores[i] > scores[best] { i } else { best }
    });

    Ok(Prediction {
        label: best,
        confidence: (scores[best] as f32 + 128.0) / 255.0,
    })
}

// ── Inference thread ───────────────────────────────────────────────────

fn infer_thread() {
    ove::log_inf!("Inference thread started — listening...");
    Thread::sleep_ms(2000);
    let mut prev_samples = SAMPLES_WRITTEN.load(Ordering::Relaxed);

    // Thread-local state
    let mut audio_window = [0i16; AUDIO_SAMPLE_FREQ as usize];
    let mut features = [[0i8; FEATURE_SIZE]; FEATURE_COUNT];
    let mut storage = ove::infer::ModelStorage::<ARENA_SIZE>::new();
    let mut dsp = DspState {
        actual_rate: AUDIO_SAMPLE_FREQ,
        dc_offset: 0,
        gain: 1,
    };

    loop {
        Thread::sleep_ms(1000);

        let cur = SAMPLES_WRITTEN.load(Ordering::Relaxed);
        let actual_rate = cur - prev_samples;
        prev_samples = cur;

        let read_count = if actual_rate > 0 {
            core::cmp::min(actual_rate, AUDIO_SAMPLE_FREQ) as usize
        } else {
            AUDIO_SAMPLE_FREQ as usize
        };

        if (AUDIO_RING.available() as usize) < read_count { continue; }
        AUDIO_RING.read_last(&mut audio_window[..read_count]);

        // Peak detection
        let peak = audio_window[..read_count].iter()
            .map(|s| s.abs())
            .max()
            .unwrap_or(0);
        ove::log_inf!("Audio: peak={}, rate={}, read={}", peak, actual_rate, read_count);

        dsp.actual_rate = if actual_rate > 0 { actual_rate } else { AUDIO_SAMPLE_FREQ };
        if peak < 10 {
            ove::log_wrn!("Audio silent — check DMIC");
            continue;
        }

        // DC offset
        let sum: i64 = audio_window[..read_count].iter().map(|&s| s as i64).sum();
        dsp.dc_offset = (sum / read_count as i64) as i32;

        // Noise gate + adaptive gain
        let dc_peak = audio_window[..read_count].iter()
            .map(|&s| (s as i32 - dsp.dc_offset).abs())
            .max()
            .unwrap_or(0);
        if dc_peak < NOISE_GATE_THRESHOLD { continue; }

        dsp.gain = (TARGET_PEAK / dc_peak).clamp(1, 200);
        ove::log_inf!("  dc_peak={}, gain={}", dc_peak, dsp.gain);

        // Inference pipeline
        if generate_features(&audio_window[..read_count], &mut features, &mut storage, &dsp).is_err() {
            ove::log_err!("Features failed");
            continue;
        }

        match classify_keyword(&features, &mut storage) {
            Ok(pred) if pred.label > 1 && pred.confidence > CONFIDENCE_THRESHOLD => {
                ove::log_inf!(">>> Keyword: \"{}\" ({:.0}%)",
                             LABELS[pred.label], pred.confidence * 100.0);
            }
            _ => {}
        }
    }
}

// ── Entry point ────────────────────────────────────────────────────────

static DMIC_PROC: ove::audio::StaticProcessor<DmicProcessor> =
    ove::audio::StaticProcessor::new(DmicProcessor);

fn app_main() {
    ove::log_inf!("=== Live DMIC Keyword Detection (Rust) ===");
    ove::log_inf!("Models: preprocessor {} + classifier {} bytes",
                 preprocessor_model().len(), classifier_model().len());

    let mut graph = ove::audio::Graph::new(512).unwrap();
    let dev_cfg = ove::audio::device_cfg_i2s(16000, 1, 1);

    let src  = graph.device_source(&dev_cfg, b"dmic-in\0").unwrap();
    let proc = graph.add_processor(DMIC_PROC.get_mut(), b"dmic-proc\0").unwrap();
    let sink = graph.device_sink(&dev_cfg, b"hp-out\0").unwrap();

    graph.connect(src, proc).unwrap();
    graph.connect(proc, sink).unwrap();
    graph.build().unwrap();
    graph.start().unwrap();
    ove::log_inf!("Audio streaming: 16kHz mono, DMIC input");

    let _infer = ove::thread!("infer", infer_thread, ove::Priority::Normal, 8192);
    ove::log_inf!("Say \"yes\" or \"no\" near the microphone...");
    ove::run();
}
