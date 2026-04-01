// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! ML inference runtime.
//!
//! Wraps the C `ove_model_*` API for running TFLite model inference.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Tensor element types.
pub const TensorType = enum(u32) {
    float32 = 0,
    int8 = 1,
    uint8 = 2,
    int16 = 3,
    int32 = 4,
};

/// Tensor descriptor with data pointer, shape, and type info.
pub const TensorInfo = struct {
    data: ?*anyopaque,
    size: usize,
    tensor_type: TensorType,
    ndims: u32,
    dims: [5]i32,
};

/// ML inference model session.
///
/// Wraps a TFLM MicroInterpreter with automatic cleanup.
pub const Model = struct {
    handle: c.ove_model_t,

    /// Create and return a new model session.
    ///
    /// In zero-heap mode, storage and arena are allocated in comptime-unique
    /// static variables — only one instance per call site.
    pub fn create(config: *const c.ove_model_config) Error!Model {
        var h: c.ove_model_t = null;
        if (comptime @hasDecl(c, "ove_model_create")) {
            try err.fromCode(c.ove_model_create(&h, config));
        } else {
            // Zero-heap: use per-call-site static storage
            const S = struct {
                var storage: c.ove_model_storage_t = std.mem.zeroes(c.ove_model_storage_t);
                var arena: [65536]u8 align(16) = std.mem.zeroes([65536]u8);
            };
            try err.fromCode(c.ove_model_init(&h, &S.storage, &S.arena, config));
        }
        return .{ .handle = h };
    }

    /// Create with a caller-provided arena buffer.
    pub fn initWithArena(
        storage: *c.ove_model_storage_t,
        arena: [*]u8,
        config: *const c.ove_model_config,
    ) Error!Model {
        var h: c.ove_model_t = null;
        try err.fromCode(c.ove_model_init(&h, storage, arena, config));
        return .{ .handle = h };
    }

    /// Destroy the model and release resources.
    pub fn destroy(self: *Model) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_model_destroy"))
            c.ove_model_destroy(self.handle)
        else
            c.ove_model_deinit(self.handle);
        self.handle = null;
    }

    /// Run the model forward pass.
    pub fn invoke(self: Model) Error!void {
        try err.fromCode(c.ove_model_invoke(self.handle));
    }

    /// Get input tensor info.
    pub fn input(self: Model, index: u32) Error!TensorInfo {
        var info: c.ove_tensor_info = std.mem.zeroes(c.ove_tensor_info);
        try err.fromCode(c.ove_model_input(self.handle, index, &info));
        return TensorInfo{
            .data = info.data,
            .size = info.size,
            .tensor_type = @enumFromInt(info.type),
            .ndims = info.ndims,
            .dims = info.dims,
        };
    }

    /// Get output tensor info.
    pub fn output(self: Model, index: u32) Error!TensorInfo {
        var info: c.ove_tensor_info = std.mem.zeroes(c.ove_tensor_info);
        try err.fromCode(c.ove_model_output(self.handle, index, &info));
        return TensorInfo{
            .data = info.data,
            .size = info.size,
            .tensor_type = @enumFromInt(info.type),
            .ndims = info.ndims,
            .dims = info.dims,
        };
    }

    /// Return last inference duration in microseconds.
    pub fn lastInferenceUs(self: Model) u64 {
        return c.ove_model_last_inference_us(self.handle);
    }

    /// Get a typed pointer to input tensor data.
    pub fn inputData(self: Model, comptime T: type, index: u32) Error![*]T {
        const info = try self.input(index);
        return @ptrCast(@alignCast(info.data));
    }

    /// Get a typed pointer to output tensor data.
    pub fn outputData(self: Model, comptime T: type, index: u32) Error![*]const T {
        const info = try self.output(index);
        return @ptrCast(@alignCast(info.data));
    }
};

/// Reusable model storage and arena pair for sequential inference.
///
/// Owns the C storage struct and an aligned arena buffer.  Call `load()`
/// to create a `Model` session.
///
/// ```zig
/// var storage = ModelArena(32768).init();
/// var model = try storage.load(&cfg);
/// defer model.destroy();
/// const input = try model.inputData(i16, 0);
/// ```
pub fn ModelArena(comptime arena_size: usize) type {
    return struct {
        storage: c.ove_model_storage_t,
        arena: [arena_size]u8 align(16),

        const Self = @This();

        pub fn init() Self {
            return .{
                .storage = std.mem.zeroes(c.ove_model_storage_t),
                .arena = std.mem.zeroes([arena_size]u8),
            };
        }

        /// Load a model from a raw C config struct.
        pub fn loadRaw(self: *Self, config: *const c.ove_model_config) Error!Model {
            return Model.initWithArena(&self.storage, &self.arena, config);
        }

        /// Load a model from a `ModelSlice` (returned by `modelSlice()`).
        ///
        /// This is the preferred API — no C types in application code.
        pub fn load(self: *Self, model_data: ModelSlice) Error!Model {
            const cfg = c.ove_model_config{
                .model_data = model_data.ptr,
                .model_size = model_data.len,
                .arena_size = arena_size,
            };
            return Model.initWithArena(&self.storage, &self.arena, &cfg);
        }
    };
}

/// Pointer + length pair for externally-linked model data.
///
/// Use with the `_model_data` / `_model_data_len` symbols generated by
/// `models/convert.py`:
///
/// ```zig
/// extern const g_my_model_data: [*]const u8;
/// extern const g_my_model_data_len: u32;
/// const my_model = ove.infer.modelSlice(g_my_model_data, g_my_model_data_len);
/// // my_model.ptr, my_model.len
/// ```
pub const ModelSlice = struct {
    ptr: [*]const u8,
    len: usize,
};

pub fn modelSlice(data: [*]const u8, len: u32) ModelSlice {
    return .{ .ptr = data, .len = @intCast(len) };
}
