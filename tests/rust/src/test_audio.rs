// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use std::sync::atomic::{AtomicI32, Ordering};
use ove::Thread;

static CALLBACK_COUNT: AtomicI32 = AtomicI32::new(0);

fn test_process_cb(out: &mut [i16], inp: &[i16]) {
    CALLBACK_COUNT.fetch_add(1, Ordering::Relaxed);
    // Simple passthrough
    let len = out.len().min(inp.len());
    out[..len].copy_from_slice(&inp[..len]);
}

fn test_init_start_stop() {
    CALLBACK_COUNT.store(0, Ordering::SeqCst);
    let cfg = ove::audio::AudioConfig {
        sample_rate: 44100,
        channels: 1,
        bit_depth: 16,
        frames_per_buffer: 256,
        thread_priority: 0,
        thread_stack_size: 0,
        num_buffers: 0,
    };
    ove::audio::init(&cfg, test_process_cb).unwrap();
    ove::audio::start().unwrap();
    Thread::sleep_ms(100);
    ove::audio::stop().unwrap();
    let count = CALLBACK_COUNT.load(Ordering::SeqCst);
    assert!(count >= 1, "callback should have fired at least once, got {}", count);
}

fn test_callback_receives_slices() {
    static SLICE_OK: AtomicI32 = AtomicI32::new(0);

    fn check_cb(out: &mut [i16], inp: &[i16]) {
        if !out.is_empty() && !inp.is_empty() && out.len() == inp.len() {
            SLICE_OK.store(1, Ordering::Release);
        }
    }

    SLICE_OK.store(0, Ordering::SeqCst);
    let cfg = ove::audio::AudioConfig {
        sample_rate: 44100,
        channels: 1,
        bit_depth: 16,
        frames_per_buffer: 128,
        thread_priority: 0,
        thread_stack_size: 0,
        num_buffers: 0,
    };
    ove::audio::init(&cfg, check_cb).unwrap();
    ove::audio::start().unwrap();
    Thread::sleep_ms(100);
    ove::audio::stop().unwrap();
    assert_eq!(SLICE_OK.load(Ordering::SeqCst), 1, "callback should receive valid slices");
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Audio",
        &[
            test_entry!(test_init_start_stop),
            test_entry!(test_callback_receives_slices),
        ],
    )
}
