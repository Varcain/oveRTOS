// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Audio subsystem for oveRTOS.
//!
//! Provides [`init`], [`start`], and [`stop`] to drive a platform audio driver
//! with a safe Rust process callback. The callback is invoked periodically by
//! the audio thread and must be real-time safe (no blocking, no allocation).

use crate::bindings;
use crate::error::{Error, Result};

/// Audio processing callback signature.
///
/// `out` — output sample buffer to fill.
/// `inp` — input sample buffer to read from.
pub type ProcessFn = fn(out: &mut [i16], inp: &[i16]);

/// Audio configuration matching `ove_audio_config`.
pub struct AudioConfig {
    /// Sample rate in Hz (e.g. 44100, 48000).
    pub sample_rate: u32,
    /// Number of audio channels (1 = mono, 2 = stereo).
    pub channels: u32,
    /// Bit depth per sample (e.g. 16, 24, 32).
    pub bit_depth: u32,
    /// Number of samples processed per callback invocation.
    pub frames_per_buffer: u32,
    /// Priority of the audio processing thread (use `ove_prio_t` values).
    pub thread_priority: u32,
    /// Stack size in bytes for the audio processing thread.
    pub thread_stack_size: u32,
    /// Number of double-buffering rings (typically 2 or 3).
    pub num_buffers: u32,
}

/// Initialize the audio subsystem with a safe Rust callback.
///
/// The `process` function is stored as user_data and invoked via a trampoline,
/// following the same pattern as `Timer`. Call [`start`] to begin audio processing.
///
/// # Errors
/// Returns an error if the hardware audio driver fails to initialize or if
/// `config` contains invalid parameters.
pub fn init(config: &AudioConfig, process: ProcessFn) -> Result<()> {
    let cfg = bindings::ove_audio_config {
        sample_rate: config.sample_rate,
        channels: config.channels,
        bit_depth: config.bit_depth,
        frames_per_buffer: config.frames_per_buffer,
        thread_priority: config.thread_priority,
        thread_stack_size: config.thread_stack_size,
        num_buffers: config.num_buffers,
    };
    let user_data = process as *mut core::ffi::c_void;
    let rc = unsafe { bindings::ove_audio_init(&cfg, Some(trampoline), user_data) };
    Error::from_code(rc)
}

/// Start audio processing (begin invoking the process callback periodically).
///
/// # Errors
/// Returns an error if the audio driver could not be started.
pub fn start() -> Result<()> {
    let rc = unsafe { bindings::ove_audio_start() };
    Error::from_code(rc)
}

/// Stop audio processing (the process callback will no longer be invoked).
///
/// # Errors
/// Returns an error if the audio driver could not be stopped.
pub fn stop() -> Result<()> {
    let rc = unsafe { bindings::ove_audio_stop() };
    Error::from_code(rc)
}

unsafe extern "C" fn trampoline(
    out: *mut i16,
    inp: *const i16,
    frame_count: u32,
    user_data: *mut core::ffi::c_void,
) {
    let cb: ProcessFn = unsafe { core::mem::transmute(user_data) };
    let out_slice = unsafe { core::slice::from_raw_parts_mut(out, frame_count as usize) };
    let in_slice = unsafe { core::slice::from_raw_parts(inp, frame_count as usize) };
    cb(out_slice, in_slice);
}
