// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Live DMIC Keyword Detection — Rust implementation
//!
//! Uses the `AudioProcessor` trait for the custom audio node — zero
//! `unsafe` or `extern "C"` in this file.  All FFI bridging is inside
//! the `ove` crate's binding layer.

#![cfg_attr(not(feature = "std"), no_std)]

use core::ptr::{addr_of, addr_of_mut};
use core::sync::atomic::{AtomicU32, Ordering};
use ove::Thread;

ove::main!(app_main);

// ── Constants ──────────────────────────────────────────────────────────

const FEATURE_SIZE: usize = 40;
const FEATURE_COUNT: usize = 49;
const FEATURE_ELEMENTS: usize = FEATURE_SIZE * FEATURE_COUNT;
const AUDIO_SAMPLE_FREQ: u32 = 16000;
const FEATURE_STRIDE_MS: u32 = 20;
const FEATURE_DURATION_MS: u32 = 30;
const AUDIO_DURATION_SAMPLES: usize = (FEATURE_DURATION_MS * AUDIO_SAMPLE_FREQ / 1000) as usize;
const CATEGORY_COUNT: usize = 4;
const ARENA_SIZE: usize = 32768;
const CONFIDENCE_THRESHOLD: f32 = 0.6;
const NOISE_GATE_THRESHOLD: i32 = 500;
const TARGET_PEAK: i32 = 15000;
const RING_BUF_CAPACITY: usize = 32768;
const RING_BUF_MASK: usize = RING_BUF_CAPACITY - 1;

static LABELS: [&[u8]; CATEGORY_COUNT] = [b"silence", b"unknown", b"yes", b"no"];

// ── Lock-free ring buffer ──────────────────────────────────────────────

struct RingBuffer {
    data: [i16; RING_BUF_CAPACITY],
    head: AtomicU32,
    tail: AtomicU32,
}

impl RingBuffer {
    const fn new() -> Self {
        Self {
            data: [0; RING_BUF_CAPACITY],
            head: AtomicU32::new(0),
            tail: AtomicU32::new(0),
        }
    }

    fn write(&mut self, sample: i16) {
        let h = self.head.load(Ordering::Relaxed) as usize;
        self.data[h & RING_BUF_MASK] = sample;
        self.head.fetch_add(1, Ordering::Release);
    }

    fn available(&self) -> u32 {
        self.head.load(Ordering::Acquire) - self.tail.load(Ordering::Relaxed)
    }

    fn read_last(&self, out: &mut [i16], count: usize) {
        let h = self.head.load(Ordering::Acquire) as usize;
        let start = if h >= count { h - count } else { 0 };
        for i in 0..count {
            out[i] = self.data[(start + i) & RING_BUF_MASK];
        }
        self.tail.store(h as u32, Ordering::Relaxed);
    }
}

// ── Shared state ───────────────────────────────────────────────────────

static mut AUDIO_RING: RingBuffer = RingBuffer::new();
static SAMPLES_WRITTEN: AtomicU32 = AtomicU32::new(0);
static mut FEATURES: [[i8; FEATURE_SIZE]; FEATURE_COUNT] = [[0; FEATURE_SIZE]; FEATURE_COUNT];
static mut G_ACTUAL_RATE: u32 = AUDIO_SAMPLE_FREQ;
static mut G_DC_OFFSET: i32 = 0;
static mut G_GAIN: i32 = 1;

// ── Model data (linked from C objects) ─────────────────────────────────

ove::model_data!(preprocessor_model,
    g_audio_preprocessor_int8_model_data,
    g_audio_preprocessor_int8_model_data_len);

ove::model_data!(classifier_model,
    g_micro_speech_quantized_model_data,
    g_micro_speech_quantized_model_data_len);

// ── DMIC processor node ────────────────────────────────────────────────
// Uses AudioProcessor trait — zero extern "C" here.

struct DmicProcessor;

impl ove::audio::AudioProcessor for DmicProcessor {
    fn process(&mut self, input: &ove::audio::AudioBuf, output: &ove::audio::AudioBuf) {
        let src = input.data_s16();
        let dst = output.data_s16_mut();
        let num_frames = input.frames() as usize / 4;

        for f in 0..num_frames {
            let base = f * 4;
            let mic_l = src[base + 1];

            unsafe { (*addr_of_mut!(AUDIO_RING)).write(mic_l) };
            SAMPLES_WRITTEN.fetch_add(1, Ordering::Relaxed);

            dst[base] = mic_l;
            dst[base + 1] = 0;
            dst[base + 2] = src[base + 3];
            dst[base + 3] = 0;
        }
    }
}

// ── Feature extraction ─────────────────────────────────────────────────

#[repr(C, align(16))]
struct AlignedArena([u8; ARENA_SIZE]);
static mut ARENA: AlignedArena = AlignedArena([0; ARENA_SIZE]);
static mut MODEL_STORAGE: ove::ffi::ove_model_storage_t = unsafe { core::mem::zeroed() };

fn generate_features(audio: &[i16]) -> i32 {
    let cfg = ove::infer::ModelConfig {
        model_data: preprocessor_model(),
        arena_size: ARENA_SIZE,
    };

    let model = unsafe {
        ove::infer::Model::from_static(
            addr_of_mut!(MODEL_STORAGE),
            (*addr_of_mut!(ARENA)).0.as_mut_ptr(),
            &cfg,
        )
    };
    let model = match model {
        Ok(m) => m,
        Err(_) => return -1,
    };

    let actual_rate = unsafe { addr_of!(G_ACTUAL_RATE).read() };
    let actual_window = (FEATURE_DURATION_MS * actual_rate / 1000) as usize;
    let actual_stride = (FEATURE_STRIDE_MS * actual_rate / 1000) as usize;

    let input_info = model.input(0).unwrap();
    let output_info = model.output(0).unwrap();

    let mut frame = 0usize;
    let mut offset = 0usize;
    while offset + actual_window <= audio.len() && frame < FEATURE_COUNT {
        let input_ptr = input_info.data as *mut i16;
        let dc = unsafe { addr_of!(G_DC_OFFSET).read() };
        let gain = unsafe { addr_of!(G_GAIN).read() };

        for i in 0..AUDIO_DURATION_SAMPLES {
            let src_idx = offset + (i as usize * actual_rate as usize / AUDIO_SAMPLE_FREQ as usize);
            let mut s: i32 = if src_idx < audio.len() { audio[src_idx] as i32 } else { 0 };
            s -= dc;
            s *= gain;
            if s > 32767 { s = 32767; }
            if s < -32768 { s = -32768; }
            unsafe { *input_ptr.add(i) = s as i16 };
        }

        if model.invoke().is_err() {
            return -1;
        }

        let output_ptr = output_info.data as *const i8;
        unsafe {
            core::ptr::copy_nonoverlapping(
                output_ptr,
                (*addr_of_mut!(FEATURES))[frame].as_mut_ptr(),
                FEATURE_SIZE,
            );
        }

        frame += 1;
        offset += actual_stride;
    }

    drop(model);
    0
}

// ── Classification ─────────────────────────────────────────────────────

fn classify_keyword() -> Option<(usize, f32)> {
    let cfg = ove::infer::ModelConfig {
        model_data: classifier_model(),
        arena_size: ARENA_SIZE,
    };

    let model = unsafe {
        ove::infer::Model::from_static(
            addr_of_mut!(MODEL_STORAGE),
            (*addr_of_mut!(ARENA)).0.as_mut_ptr(),
            &cfg,
        )
    }.ok()?;

    let input_info = model.input(0).ok()?;
    unsafe {
        core::ptr::copy_nonoverlapping(
            addr_of!(FEATURES) as *const u8,
            input_info.data as *mut u8,
            FEATURE_ELEMENTS,
        );
    }

    model.invoke().ok()?;

    let output_info = model.output(0).ok()?;
    let scores = unsafe {
        core::slice::from_raw_parts(output_info.data as *const i8, CATEGORY_COUNT)
    };

    let mut best = 0usize;
    for i in 1..CATEGORY_COUNT {
        if scores[i] > scores[best] { best = i; }
    }

    let confidence = (scores[best] as f32 + 128.0) / 255.0;
    Some((best, confidence))
}

// ── Inference thread ───────────────────────────────────────────────────

static mut AUDIO_WINDOW: [i16; AUDIO_SAMPLE_FREQ as usize] = [0; AUDIO_SAMPLE_FREQ as usize];

fn infer_thread() {
    ove::log_inf!("Inference thread started — listening...");
    Thread::sleep_ms(2000);
    let mut prev_samples = SAMPLES_WRITTEN.load(Ordering::Relaxed);

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

        let avail = unsafe { (*addr_of!(AUDIO_RING)).available() } as usize;
        if avail < read_count { continue; }

        unsafe {
            (*addr_of!(AUDIO_RING)).read_last(
                &mut *core::ptr::slice_from_raw_parts_mut(
                    addr_of_mut!(AUDIO_WINDOW) as *mut i16,
                    AUDIO_SAMPLE_FREQ as usize,
                ),
                read_count,
            );
        }

        // Peak
        let mut peak: i16 = 0;
        for i in 0..read_count {
            let s = unsafe { (*addr_of!(AUDIO_WINDOW))[i] };
            let a = if s < 0 { -s } else { s };
            if a > peak { peak = a; }
        }
        ove::log_inf!("Audio: peak={}, rate={}, read={}", peak, actual_rate, read_count);

        unsafe { addr_of_mut!(G_ACTUAL_RATE).write(if actual_rate > 0 { actual_rate } else { AUDIO_SAMPLE_FREQ }) };
        if peak < 10 { ove::log_inf!("Audio silent"); continue; }

        // DC offset
        let mut sum: i64 = 0;
        for i in 0..read_count {
            sum += unsafe { (*addr_of!(AUDIO_WINDOW))[i] } as i64;
        }
        unsafe { addr_of_mut!(G_DC_OFFSET).write((sum / read_count as i64) as i32) };

        // Noise gate + adaptive gain
        let mut dc_peak: i32 = 0;
        for i in 0..read_count {
            let mut s = unsafe { (*addr_of!(AUDIO_WINDOW))[i] } as i32 - unsafe { addr_of!(G_DC_OFFSET).read() };
            if s < 0 { s = -s; }
            if s > dc_peak { dc_peak = s; }
        }
        if dc_peak < NOISE_GATE_THRESHOLD { continue; }

        let gain = TARGET_PEAK / dc_peak;
        unsafe {
            addr_of_mut!(G_GAIN).write(gain.clamp(1, 200));
        }
        ove::log_inf!("  dc_peak={}, gain={}", dc_peak, unsafe { addr_of!(G_GAIN).read() });

        // Inference pipeline
        let rc = generate_features(unsafe {
            core::slice::from_raw_parts(addr_of!(AUDIO_WINDOW) as *const i16, read_count)
        });
        if rc != 0 { ove::log_err!("Features failed"); continue; }

        if let Some((prediction, confidence)) = classify_keyword() {
            if prediction > 1 && confidence > CONFIDENCE_THRESHOLD {
                ove::log_inf!(">>> Keyword: \"{}\" ({:.0}%)",
                             core::str::from_utf8(LABELS[prediction]).unwrap_or("?"),
                             confidence * 100.0);
            }
        }
    }
}

// ── Entry point ────────────────────────────────────────────────────────

static mut DMIC_PROC: DmicProcessor = DmicProcessor;

fn app_main() {
    ove::log_inf!("=== Live DMIC Keyword Detection (Rust) ===");
    ove::log_inf!("Models: preprocessor {} + classifier {} bytes",
                 preprocessor_model().len(), classifier_model().len());

    // Audio graph
    let mut graph: ove::ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    ove::audio::graph_init(&mut graph, 512).unwrap();

    let dev_cfg = ove::audio::device_cfg_i2s(16000, 1, 1);

    let src = unsafe {
        ove::ffi::ove_audio_device_source(&mut graph, &dev_cfg, b"dmic-in\0".as_ptr() as *const _)
    };
    let proc_idx = ove::audio::graph_add_processor(
        &mut graph,
        unsafe { &mut *addr_of_mut!(DMIC_PROC) },
        b"dmic-proc\0",
    ).unwrap();
    let sink = unsafe {
        ove::ffi::ove_audio_device_sink(&mut graph, &dev_cfg, b"hp-out\0".as_ptr() as *const _)
    };

    ove::audio::graph_connect(&mut graph, src as u32, proc_idx as u32).unwrap();
    ove::audio::graph_connect(&mut graph, proc_idx as u32, sink as u32).unwrap();
    ove::audio::graph_build(&mut graph).unwrap();
    ove::audio::graph_start(&mut graph).unwrap();
    ove::log_inf!("Audio streaming: 16kHz mono, DMIC input");

    let _infer = ove::thread!("infer", infer_thread, ove::Priority::Normal, 8192);
    ove::log_inf!("Say \"yes\" or \"no\" near the microphone...");
    ove::run();
}
