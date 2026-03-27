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
        if (rc < 0) return err.fromCodeRaw(rc);
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
        if (rc < 0) return err.fromCodeRaw(rc);
        return rc;
    }

    pub fn deviceSink(
        self: *Graph,
        cfg: *const c.struct_ove_audio_device_cfg,
        name: [*:0]const u8,
    ) Error!i32 {
        const rc = c.ove_audio_device_sink(&self.raw, cfg, name);
        if (rc < 0) return err.fromCodeRaw(rc);
        return rc;
    }
};
