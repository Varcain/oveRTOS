// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Configuration for the audio subsystem.
///
/// Zero-valued fields are replaced by driver defaults at `init()` time.
pub const Config = struct {
    /// Audio sample rate in Hz (e.g. 44100, 48000). Default: 44100.
    sample_rate: u32 = 44100,
    /// Number of audio channels (1 = mono, 2 = stereo). Default: 1.
    channels: u32 = 1,
    /// Bits per sample (typically 16 or 32). Default: 16.
    bit_depth: u32 = 16,
    /// Number of frames processed per callback invocation. Default: 256.
    frames_per_buffer: u32 = 256,
    /// Priority of the audio processing thread. 0 means driver default.
    thread_priority: u32 = 0,
    /// Stack size of the audio processing thread in bytes. 0 means driver default.
    thread_stack_size: u32 = 0,
    /// Number of DMA ping-pong buffers. 0 means driver default.
    num_buffers: u32 = 0,
};

/// Initialize the audio subsystem with the given configuration and processing callback.
///
/// `process` is called periodically by the audio driver thread. It receives
/// a pointer to the output buffer (`out`), input buffer (`in_`), and the
/// number of frames to process. The callback must not block.
///
/// Returns `Error` if the underlying driver fails to initialize.
pub fn init(
    cfg: Config,
    comptime process: fn (out: [*]i16, in_: [*]const i16, frames: u32) void,
) Error!void {
    const Trampoline = struct {
        fn invoke(
            out: [*c]i16,
            in_: [*c]const i16,
            frames: c_uint,
            _: ?*anyopaque,
        ) callconv(.c) void {
            process(out, in_, frames);
        }
    };

    var raw_cfg: c.struct_ove_audio_config = .{
        .sample_rate = cfg.sample_rate,
        .channels = cfg.channels,
        .bit_depth = cfg.bit_depth,
        .frames_per_buffer = cfg.frames_per_buffer,
        .thread_priority = cfg.thread_priority,
        .thread_stack_size = cfg.thread_stack_size,
        .num_buffers = cfg.num_buffers,
    };
    try err.fromCode(c.ove_audio_init(&raw_cfg, &Trampoline.invoke, null));
}

/// Start audio streaming. The processing callback begins receiving frames.
///
/// Returns `Error` if the driver is not initialized or start fails.
pub fn start() Error!void {
    try err.fromCode(c.ove_audio_start());
}

/// Stop audio streaming and silence the output.
///
/// Returns `Error` if the driver reports a failure.
pub fn stop() Error!void {
    try err.fromCode(c.ove_audio_stop());
}

/// Pause audio streaming without releasing driver resources.
///
/// The processing callback stops receiving frames. Resume with `resume_()`.
/// Returns `Error` if the driver reports a failure.
pub fn pause() Error!void {
    try err.fromCode(c.ove_audio_pause());
}

/// Resume audio streaming after a `pause()` call.
///
/// Returns `Error` if the driver reports a failure.
pub fn resume_() Error!void {
    try err.fromCode(c.ove_audio_resume());
}

/// Shut down the audio subsystem and release all driver resources.
pub fn deinit() void {
    c.ove_audio_deinit();
}
