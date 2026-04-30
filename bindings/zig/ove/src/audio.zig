// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Audio graph engine bindings for Zig.
//!
//! Build a DAG of audio nodes, validate formats, and execute in
//! topological order — either sink-driven or app-driven.

const c = @import("c.zig").raw;
const std = @import("std");
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

/// Audio graph wrapper.  Embeds the C `ove_audio_graph` directly.
///
/// ```zig
/// var graph: ove.audio.Graph = undefined;
/// try graph.init(frames_per_period);
/// defer graph.deinit();
///
/// // Zero-heap: caller supplies the inter-node sample buffer pool.
/// var sample_pool: [N_NODES * FRAMES * CHANNELS * @sizeOf(i16)]u8 align(4) = undefined;
/// try graph.setBufStorage(&sample_pool);
/// ```
///
/// In heap mode `setBufStorage` is unnecessary — the engine allocates
/// per-edge buffers internally.
pub const Graph = struct {
    raw: c.struct_ove_audio_graph,
    tracker: pin.Tracker,

    pub fn init(self: *Graph, frames_per_period: u32) Error!void {
        self.raw = std.mem.zeroes(c.struct_ove_audio_graph);
        self.tracker = .{};
        try err.fromCode(c.ove_audio_graph_init(&self.raw, frames_per_period));
        self.tracker.record(self);
    }

    /// Attach caller-supplied scratch storage for inter-node sample buffers.
    /// Required under `CONFIG_OVE_ZERO_HEAP=y`; ignored in heap mode.
    pub fn setBufStorage(self: *Graph, bytes: []u8) Error!void {
        self.tracker.assertSame(self, "ove.audio.Graph");
        try err.fromCode(c.ove_audio_graph_set_buf_storage(&self.raw, bytes.ptr, bytes.len));
    }

    pub fn deinit(self: *Graph) void {
        self.tracker.assertSame(self, "ove.audio.Graph");
        c.ove_audio_graph_deinit(&self.raw);
        self.tracker.clear();
    }

    pub fn addNode(
        self: *Graph,
        ops: *const c.struct_ove_audio_node_ops,
        ctx: ?*anyopaque,
        name: [*:0]const u8,
        node_type: c_uint,
    ) Error!i32 {
        const rc = c.ove_audio_graph_add_node(&self.raw, ops, ctx, name, node_type);
        if (rc < 0) return err.fromCodeInt(rc);
        return rc;
    }

    pub fn connect(self: *Graph, from: u32, to: u32) Error!void {
        try err.fromCode(c.ove_audio_graph_connect(&self.raw, from, to));
    }

    pub fn build(self: *Graph) Error!void {
        try err.fromCode(c.ove_audio_graph_build(&self.raw));
    }

    pub fn start(self: *Graph) Error!void {
        try err.fromCode(c.ove_audio_graph_start(&self.raw));
    }

    pub fn stop(self: *Graph) Error!void {
        try err.fromCode(c.ove_audio_graph_stop(&self.raw));
    }

    pub fn process(self: *Graph) Error!void {
        try err.fromCode(c.ove_audio_graph_process(&self.raw));
    }

    pub fn deviceSource(
        self: *Graph,
        cfg: *const c.struct_ove_audio_device_cfg,
        name: [*:0]const u8,
    ) Error!i32 {
        const rc = c.ove_audio_device_source(&self.raw, cfg, name);
        if (rc < 0) return err.fromCodeInt(rc);
        return rc;
    }

    pub fn deviceSink(
        self: *Graph,
        cfg: *const c.struct_ove_audio_device_cfg,
        name: [*:0]const u8,
    ) Error!i32 {
        const rc = c.ove_audio_device_sink(&self.raw, cfg, name);
        if (rc < 0) return err.fromCodeInt(rc);
        return rc;
    }
    // -----------------------------------------------------------------
    // Device configuration helpers
    // -----------------------------------------------------------------

    /// Build an I2S device configuration.
    /// Hides the anonymous union from application code.
    pub fn deviceCfgI2s(sample_rate: u32, channels: u32, input_device: u32) c.struct_ove_audio_device_cfg {
        var cfg = std.mem.zeroes(c.struct_ove_audio_device_cfg);
        cfg.transport = c.OVE_AUDIO_TRANSPORT_I2S;
        cfg.fmt.sample_rate = sample_rate;
        cfg.fmt.channels = channels;
        cfg.fmt.sample_fmt = c.OVE_AUDIO_FMT_S16;
        // Write input_device into the anonymous union at its known offset
        const union_bytes: [*]u8 = @ptrCast(&cfg.unnamed_0);
        const dev_ptr: *align(1) u32 = @ptrCast(union_bytes);
        dev_ptr.* = input_device;
        return cfg;
    }

    // -----------------------------------------------------------------
    // Safe processor node registration (comptime trampoline)
    // -----------------------------------------------------------------

    /// Register a custom processor node using a typed Zig struct.
    ///
    /// `NodeType` must have a method with this signature:
    /// ```zig
    /// pub fn process(self: *NodeType, input: AudioBuf, output: AudioBuf) void
    /// ```
    /// The binding layer generates `callconv(.c)` trampolines at comptime
    /// and wraps raw C buffers in safe `AudioBuf` types — no `@ptrCast`
    /// needed in application code.
    pub fn addProcessor(
        self: *Graph,
        comptime NodeType: type,
        node: *NodeType,
        name: [*:0]const u8,
    ) Error!i32 {
        const Ops = struct {
            fn configure(
                _: ?*anyopaque,
                in_fmt: [*c]const c.struct_ove_audio_fmt,
                out_fmt: [*c]c.struct_ove_audio_fmt,
            ) callconv(.c) c_int {
                if (in_fmt != null and out_fmt != null) {
                    out_fmt.* = in_fmt.*;
                }
                return 0;
            }

            fn invoke(
                ctx: ?*anyopaque,
                in_buf: [*c]const c.struct_ove_audio_buf,
                out_buf: [*c]c.struct_ove_audio_buf,
            ) callconv(.c) c_int {
                const n: *NodeType = @ptrCast(@alignCast(ctx));
                n.process(AudioBuf{ .raw = in_buf }, AudioBuf{ .raw = out_buf });
                return 0;
            }

            const vtable = c.struct_ove_audio_node_ops{
                .configure = &configure,
                .start = null,
                .stop = null,
                .process = &invoke,
                .destroy = null,
            };
        };
        const rc = c.ove_audio_graph_add_node(
            &self.raw,
            &Ops.vtable,
            @ptrCast(node),
            name,
            c.OVE_AUDIO_NODE_PROCESSOR,
        );
        if (rc < 0) return err.fromCodeInt(rc);
        return rc;
    }
};

// ---------------------------------------------------------------------------
// Safe audio buffer wrapper
// ---------------------------------------------------------------------------

/// Safe wrapper around the C `ove_audio_buf`.
///
/// Provides typed slice accessors so application code never needs
/// `@ptrCast` or `@alignCast` on raw buffer data.
pub const AudioBuf = struct {
    raw: [*c]const c.struct_ove_audio_buf,

    /// Number of frames in this buffer.
    pub fn frames(self: AudioBuf) u32 {
        return self.raw.*.frames;
    }

    /// Number of interleaved channels in this buffer.
    pub fn channels(self: AudioBuf) u32 {
        return self.raw.*.fmt.*.channels;
    }

    /// Total sample count (frames * channels).
    fn sampleCount(self: AudioBuf) usize {
        return self.raw.*.frames * self.raw.*.fmt.*.channels;
    }

    /// Get interleaved S16 samples as a read-only slice.
    pub fn dataS16(self: AudioBuf) []const i16 {
        const count = self.sampleCount();
        const ptr: [*]const i16 = @ptrCast(@alignCast(self.raw.*.data));
        return ptr[0..count];
    }

    /// Get interleaved S16 samples as a mutable slice.
    pub fn dataS16Mut(self: AudioBuf) []i16 {
        const count = self.sampleCount();
        const ptr: [*]i16 = @ptrCast(@alignCast(self.raw.*.data));
        return ptr[0..count];
    }
};
