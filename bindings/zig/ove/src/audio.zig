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

/// Audio graph wrapper.
pub const Graph = struct {
    raw: c.struct_ove_audio_graph,

    pub fn init(frames_per_period: u32) Error!Graph {
        var g: Graph = undefined;
        try err.fromCode(c.ove_audio_graph_init(&g.raw, frames_per_period));
        return g;
    }

    pub fn deinit(self: *Graph) void {
        c.ove_audio_graph_deinit(&self.raw);
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
    /// `NodeType` must have a `pub fn process(self: *NodeType, ...)` method.
    /// The binding layer generates `callconv(.c)` trampolines at comptime —
    /// no raw C callbacks needed in application code.
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
                n.process(in_buf, out_buf);
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
