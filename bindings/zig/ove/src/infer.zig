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
const pin = @import("pin.zig");

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
/// In heap mode the wrapper is a single handle; the kernel allocates the
/// storage and arena from the heap:
///
/// ```zig
/// var model = try ove.Model.create(&model_cfg);
/// defer model.deinit();
/// try model.invoke();
/// ```
///
/// In zero-heap mode the storage struct lives in the wrapper; the arena
/// (working memory for the interpreter) is **not** embedded — its size
/// depends on the model and would explode the wrapper's footprint; callers
/// supply an aligned arena buffer via `init()`'s `arena` slice:
///
/// ```zig
/// var arena: [32 * 1024]u8 align(16) = undefined;
/// var model: ove.Model = undefined;
/// try model.init(&arena, &model_cfg);
/// defer model.deinit();
/// try model.invoke();
/// ```
pub const Model = if (pin.zero_heap) ZeroHeapModel else HeapModel;

const HeapModel = struct {
    handle: c.ove_model_t,

    /// Create.  Storage and arena are allocated from the RTOS heap.
    pub fn create(config: *const c.ove_model_config) Error!Model {
        var h: c.ove_model_t = null;
        try err.fromCode(c.ove_model_create(&h, config));
        return .{ .handle = h };
    }

    pub fn deinit(self: Model) void {
        if (self.handle == null) return;
        c.ove_model_destroy(self.handle);
    }

    /// Run the model forward pass.
    pub fn invoke(self: Model) Error!void {
        try err.fromCode(c.ove_model_invoke(self.handle));
    }

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

    pub fn lastInferenceUs(self: Model) u64 {
        return c.ove_model_last_inference_us(self.handle);
    }

    pub fn inputData(self: Model, comptime T: type, index: u32) Error![*]T {
        const info = try self.input(index);
        return @ptrCast(@alignCast(info.data));
    }

    pub fn outputData(self: Model, comptime T: type, index: u32) Error![*]const T {
        const info = try self.output(index);
        return @ptrCast(@alignCast(info.data));
    }
};

const ZeroHeapModel = struct {
    storage: c.ove_model_storage_t,
    handle: c.ove_model_t,
    tracker: pin.Tracker,

    /// Initialise.  `arena` is caller-supplied scratch memory used by the
    /// interpreter (sized per-model).
    pub fn init(self: *Model, arena: []u8, config: *const c.ove_model_config) Error!void {
        self.storage = std.mem.zeroes(c.ove_model_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_model_init(&self.handle, &self.storage, arena.ptr, config));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Model) void {
        self.tracker.assertSame(self, "ove.Model");
        if (self.handle == null) return;
        c.ove_model_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    /// Run the model forward pass.
    pub fn invoke(self: *Model) Error!void {
        self.tracker.assertSame(self, "ove.Model");
        try err.fromCode(c.ove_model_invoke(self.handle));
    }

    pub fn input(self: *Model, index: u32) Error!TensorInfo {
        self.tracker.assertSame(self, "ove.Model");
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

    pub fn output(self: *Model, index: u32) Error!TensorInfo {
        self.tracker.assertSame(self, "ove.Model");
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

    pub fn lastInferenceUs(self: *Model) u64 {
        self.tracker.assertSame(self, "ove.Model");
        return c.ove_model_last_inference_us(self.handle);
    }

    pub fn inputData(self: *Model, comptime T: type, index: u32) Error![*]T {
        const info = try self.input(index);
        return @ptrCast(@alignCast(info.data));
    }

    pub fn outputData(self: *Model, comptime T: type, index: u32) Error![*]const T {
        const info = try self.output(index);
        return @ptrCast(@alignCast(info.data));
    }
};

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
