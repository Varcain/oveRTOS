// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use core::ffi::{c_int, c_void};
use ove::audio::{
    device_cfg_i2s, graph_add_processor, graph_build, graph_connect, graph_deinit, graph_init,
    graph_process, graph_start, graph_stop, AudioBuf, AudioProcessor, Graph, SampleFmt,
    StaticProcessor,
};
use ove::ffi;
use ove::Error;
use std::sync::atomic::{AtomicU32, Ordering};

/* ── Existing low-level tests ───────────────────────────────────── */

fn test_graph_init_deinit() {
    let mut graph: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    graph_init(&mut graph, 256).unwrap();
    graph_deinit(&mut graph);
}

fn test_graph_init_zero_frames() {
    let mut graph: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    assert!(graph_init(&mut graph, 0).is_err());
}

fn test_graph_build_empty() {
    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    graph_init(&mut g, 256).unwrap();
    graph_build(&mut g).unwrap();
    graph_deinit(&mut g);
}

fn test_graph_connect_no_nodes() {
    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    graph_init(&mut g, 256).unwrap();
    assert!(graph_connect(&mut g, 0, 0).is_err());
    graph_deinit(&mut g);
}

fn test_graph_start_not_ready() {
    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    graph_init(&mut g, 256).unwrap();
    assert!(graph_start(&mut g).is_err());
    graph_deinit(&mut g);
}

/* ── SampleFmt ──────────────────────────────────────────────────── */

fn test_sample_fmt_to_raw_all_variants() {
    assert_eq!(SampleFmt::S16.to_raw(), 0);
    assert_eq!(SampleFmt::S32.to_raw(), 1);
    assert_eq!(SampleFmt::F32.to_raw(), 2);
}

fn test_sample_fmt_copy_eq() {
    let a = SampleFmt::S16;
    let b = a;
    assert_eq!(a, b);
    assert_ne!(SampleFmt::S16, SampleFmt::S32);
    assert_ne!(SampleFmt::S32, SampleFmt::F32);
}

/* ── device_cfg_i2s ─────────────────────────────────────────────── */

fn test_device_cfg_i2s_sets_transport_and_fmt() {
    let cfg = device_cfg_i2s(16_000, 1, 0);
    assert_eq!(cfg.transport, ffi::OVE_AUDIO_TRANSPORT_I2S);
    assert_eq!(cfg.fmt.sample_rate, 16_000);
    assert_eq!(cfg.fmt.channels, 1);
    assert_eq!(cfg.fmt.sample_fmt, ffi::OVE_AUDIO_FMT_S16);
}

fn test_device_cfg_i2s_multichannel() {
    let cfg = device_cfg_i2s(48_000, 2, 7);
    assert_eq!(cfg.fmt.sample_rate, 48_000);
    assert_eq!(cfg.fmt.channels, 2);
}

/* ── Graph safe wrapper ─────────────────────────────────────────── */

fn test_graph_wrapper_new_drop() {
    let _g = Graph::new(256).unwrap();
}

fn test_graph_wrapper_new_zero_frames_fails() {
    assert!(Graph::new(0).is_err());
}

fn test_graph_wrapper_stop_not_running() {
    let mut g = Graph::new(128).unwrap();
    assert!(matches!(g.stop(), Err(Error::NotSupported)));
}

fn test_graph_wrapper_process_not_built() {
    let mut g = Graph::new(128).unwrap();
    assert!(matches!(g.process(), Err(Error::NotSupported)));
}

fn test_graph_wrapper_connect_invalid_index() {
    let mut g = Graph::new(128).unwrap();
    assert!(g.connect(0, 1).is_err());
}

fn test_graph_wrapper_start_not_ready() {
    let mut g = Graph::new(128).unwrap();
    assert!(g.start().is_err());
}

fn test_graph_wrapper_build_empty_succeeds() {
    let mut g = Graph::new(128).unwrap();
    g.build().unwrap();
}

/* ── StaticProcessor ────────────────────────────────────────────── */

fn test_static_processor_get_mut() {
    static SP: StaticProcessor<i32> = StaticProcessor::new(7);
    let r = SP.get_mut();
    assert_eq!(*r, 7);
    *r = 99;
    // NLL: `r` not reused after assignment; new borrow is well-defined.
    assert_eq!(*SP.get_mut(), 99);
}

/* ── Full pipeline: source → AudioProcessor → sink ──────────────── */

struct Passthrough;

static PROC_CALLS: AtomicU32 = AtomicU32::new(0);
static OBSERVED_FRAMES: AtomicU32 = AtomicU32::new(0);
static OBSERVED_CHANNELS: AtomicU32 = AtomicU32::new(0);
static OBSERVED_FIRST_SAMPLE: AtomicU32 = AtomicU32::new(0);

impl AudioProcessor for Passthrough {
    fn process(&mut self, input: &AudioBuf, output: &AudioBuf) {
        PROC_CALLS.fetch_add(1, Ordering::SeqCst);
        OBSERVED_FRAMES.store(input.frames(), Ordering::SeqCst);
        OBSERVED_CHANNELS.store(input.channels(), Ordering::SeqCst);
        let src = input.data_s16();
        let dst = output.data_s16_mut();
        if !src.is_empty() {
            // Store as u32 so we avoid signed-to-unsigned cast issues for
            // the small positive test values we produce in the source.
            OBSERVED_FIRST_SAMPLE.store(src[0] as u32, Ordering::SeqCst);
        }
        dst.copy_from_slice(src);
    }
}

static PROC: StaticProcessor<Passthrough> = StaticProcessor::new(Passthrough);

// ── Custom source node ops (raw C vtable) ──
unsafe extern "C" fn src_configure(
    _ctx: *mut c_void,
    _in_fmt: *const ffi::ove_audio_fmt,
    out_fmt: *mut ffi::ove_audio_fmt,
) -> c_int {
    if !out_fmt.is_null() {
        unsafe {
            (*out_fmt).sample_rate = 16_000;
            (*out_fmt).channels = 1;
            (*out_fmt).sample_fmt = ffi::OVE_AUDIO_FMT_S16;
        }
    }
    0
}

static SRC_START_CALLS: AtomicU32 = AtomicU32::new(0);
static SRC_STOP_CALLS: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn src_start(_ctx: *mut c_void) -> c_int {
    SRC_START_CALLS.fetch_add(1, Ordering::SeqCst);
    0
}
unsafe extern "C" fn src_stop(_ctx: *mut c_void) -> c_int {
    SRC_STOP_CALLS.fetch_add(1, Ordering::SeqCst);
    0
}
unsafe extern "C" fn src_process(
    _ctx: *mut c_void,
    _in_buf: *const ffi::ove_audio_buf,
    out_buf: *mut ffi::ove_audio_buf,
) -> c_int {
    if !out_buf.is_null() {
        unsafe {
            let buf = &*out_buf;
            let count = buf.frames as usize * (*buf.fmt).channels as usize;
            let data = buf.data as *mut i16;
            for i in 0..count {
                *data.add(i) = (i as i16) + 42;
            }
        }
    }
    0
}

static SRC_OPS: ffi::ove_audio_node_ops = ffi::ove_audio_node_ops {
    configure: Some(src_configure),
    start: Some(src_start),
    stop: Some(src_stop),
    process: Some(src_process),
    destroy: None,
};

// ── Custom sink node ops ──
static SINK_PROCESS_CALLS: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn snk_configure(
    _ctx: *mut c_void,
    _in_fmt: *const ffi::ove_audio_fmt,
    _out_fmt: *mut ffi::ove_audio_fmt,
) -> c_int {
    0
}
unsafe extern "C" fn snk_process(
    _ctx: *mut c_void,
    _in_buf: *const ffi::ove_audio_buf,
    _out_buf: *mut ffi::ove_audio_buf,
) -> c_int {
    SINK_PROCESS_CALLS.fetch_add(1, Ordering::SeqCst);
    0
}

static SINK_OPS: ffi::ove_audio_node_ops = ffi::ove_audio_node_ops {
    configure: Some(snk_configure),
    start: None,
    stop: None,
    process: Some(snk_process),
    destroy: None,
};

fn build_pipeline(g: &mut ffi::ove_audio_graph) {
    graph_init(g, 64).unwrap();
    let src = unsafe {
        ffi::ove_audio_graph_add_node(
            g,
            &SRC_OPS,
            core::ptr::null_mut(),
            b"src\0".as_ptr() as *const _,
            ffi::OVE_AUDIO_NODE_SOURCE,
        )
    };
    assert!(src >= 0, "add source failed: {src}");
    let prc = graph_add_processor(g, PROC.get_mut(), b"prc\0").unwrap();
    let snk = unsafe {
        ffi::ove_audio_graph_add_node(
            g,
            &SINK_OPS,
            core::ptr::null_mut(),
            b"snk\0".as_ptr() as *const _,
            ffi::OVE_AUDIO_NODE_SINK,
        )
    };
    assert!(snk >= 0, "add sink failed: {snk}");
    graph_connect(g, src as u32, prc as u32).unwrap();
    graph_connect(g, prc as u32, snk as u32).unwrap();
    graph_build(g).unwrap();
}

fn test_full_pipeline_process_runs_processor() {
    PROC_CALLS.store(0, Ordering::SeqCst);
    SINK_PROCESS_CALLS.store(0, Ordering::SeqCst);
    OBSERVED_FRAMES.store(0, Ordering::SeqCst);
    OBSERVED_CHANNELS.store(0, Ordering::SeqCst);
    OBSERVED_FIRST_SAMPLE.store(0, Ordering::SeqCst);

    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    build_pipeline(&mut g);
    graph_process(&mut g).unwrap();
    graph_process(&mut g).unwrap();

    assert_eq!(PROC_CALLS.load(Ordering::SeqCst), 2);
    assert_eq!(SINK_PROCESS_CALLS.load(Ordering::SeqCst), 2);
    assert_eq!(OBSERVED_FRAMES.load(Ordering::SeqCst), 64);
    assert_eq!(OBSERVED_CHANNELS.load(Ordering::SeqCst), 1);
    // Source writes sample[0] = 42, processor observes it.
    assert_eq!(OBSERVED_FIRST_SAMPLE.load(Ordering::SeqCst), 42);

    graph_deinit(&mut g);
}

fn test_full_pipeline_start_stop_calls_node_ops() {
    SRC_START_CALLS.store(0, Ordering::SeqCst);
    SRC_STOP_CALLS.store(0, Ordering::SeqCst);

    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    build_pipeline(&mut g);
    graph_start(&mut g).unwrap();
    assert_eq!(SRC_START_CALLS.load(Ordering::SeqCst), 1);

    // Process while running — exercises the RUNNING state in process().
    graph_process(&mut g).unwrap();

    graph_stop(&mut g).unwrap();
    assert_eq!(SRC_STOP_CALLS.load(Ordering::SeqCst), 1);

    graph_deinit(&mut g);
}

fn test_full_pipeline_stop_before_start_fails() {
    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    build_pipeline(&mut g);
    // READY but not RUNNING — stop must fail.
    assert!(matches!(graph_stop(&mut g), Err(Error::NotSupported)));
    graph_deinit(&mut g);
}

/* ── graph_add_node free function ───────────────────────────────── */

fn test_graph_add_node_free_fn_ok() {
    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    graph_init(&mut g, 128).unwrap();
    let name = core::ffi::CStr::from_bytes_with_nul(b"free_src\0").unwrap();
    let idx = ove::audio::graph_add_node(
        &mut g,
        &SRC_OPS,
        core::ptr::null_mut(),
        name,
        ffi::OVE_AUDIO_NODE_SOURCE,
    )
    .unwrap();
    assert!(idx >= 0);
    graph_deinit(&mut g);
}

fn test_graph_add_node_free_fn_error_after_build() {
    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    graph_init(&mut g, 128).unwrap();
    graph_build(&mut g).unwrap(); // graph leaves IDLE → READY
    let name = core::ffi::CStr::from_bytes_with_nul(b"too_late\0").unwrap();
    let result = ove::audio::graph_add_node(
        &mut g,
        &SRC_OPS,
        core::ptr::null_mut(),
        name,
        ffi::OVE_AUDIO_NODE_SOURCE,
    );
    assert!(result.is_err());
    graph_deinit(&mut g);
}

fn test_graph_add_processor_error_after_build() {
    let mut g: ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    graph_init(&mut g, 128).unwrap();
    graph_build(&mut g).unwrap();
    // After build() the graph is READY; add_processor requires IDLE → error branch.
    let result = graph_add_processor(&mut g, PROC.get_mut(), b"late\0");
    assert!(result.is_err());
    graph_deinit(&mut g);
}

/* ── Graph::device_source / device_sink ───────────────────────────
 *
 * Backed by `tests/backends/stub/stub_audio_device.c`, which validates
 * args and returns a dummy node index (0) on success or INVALID_PARAM
 * when the C name string is empty.  The real implementation in
 * sim/hal/sim_audio.c depends on miniaudio and is not linked in
 * ove_stub.
 */

fn test_graph_wrapper_device_source_ok() {
    let mut g = Graph::new(128).unwrap();
    let cfg = device_cfg_i2s(16_000, 1, 0);
    let _ = g.device_source(&cfg, b"mic\0").unwrap();
}

fn test_graph_wrapper_device_sink_ok() {
    let mut g = Graph::new(128).unwrap();
    let cfg = device_cfg_i2s(16_000, 1, 0);
    let _ = g.device_sink(&cfg, b"spk\0").unwrap();
}

fn test_graph_wrapper_device_source_err() {
    // Empty C string (first byte is NUL) — the stub rejects it with
    // INVALID_PARAM, exercising the `if rc < 0 { Err(...) }` branch.
    let mut g = Graph::new(128).unwrap();
    let cfg = device_cfg_i2s(16_000, 1, 0);
    let result = g.device_source(&cfg, b"\0");
    assert!(matches!(result, Err(Error::InvalidParam)), "got {:?}", result);
}

fn test_graph_wrapper_device_sink_err() {
    let mut g = Graph::new(128).unwrap();
    let cfg = device_cfg_i2s(16_000, 1, 0);
    let result = g.device_sink(&cfg, b"\0");
    assert!(matches!(result, Err(Error::InvalidParam)), "got {:?}", result);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Audio",
        &[
            test_entry!(test_graph_init_deinit),
            test_entry!(test_graph_init_zero_frames),
            test_entry!(test_graph_build_empty),
            test_entry!(test_graph_connect_no_nodes),
            test_entry!(test_graph_start_not_ready),
            test_entry!(test_sample_fmt_to_raw_all_variants),
            test_entry!(test_sample_fmt_copy_eq),
            test_entry!(test_device_cfg_i2s_sets_transport_and_fmt),
            test_entry!(test_device_cfg_i2s_multichannel),
            test_entry!(test_graph_wrapper_new_drop),
            test_entry!(test_graph_wrapper_new_zero_frames_fails),
            test_entry!(test_graph_wrapper_stop_not_running),
            test_entry!(test_graph_wrapper_process_not_built),
            test_entry!(test_graph_wrapper_connect_invalid_index),
            test_entry!(test_graph_wrapper_start_not_ready),
            test_entry!(test_graph_wrapper_build_empty_succeeds),
            test_entry!(test_static_processor_get_mut),
            test_entry!(test_full_pipeline_process_runs_processor),
            test_entry!(test_full_pipeline_start_stop_calls_node_ops),
            test_entry!(test_full_pipeline_stop_before_start_fails),
            test_entry!(test_graph_add_node_free_fn_ok),
            test_entry!(test_graph_add_node_free_fn_error_after_build),
            test_entry!(test_graph_add_processor_error_after_build),
            test_entry!(test_graph_wrapper_device_source_ok),
            test_entry!(test_graph_wrapper_device_sink_ok),
            test_entry!(test_graph_wrapper_device_source_err),
            test_entry!(test_graph_wrapper_device_sink_err),
        ],
    )
}
