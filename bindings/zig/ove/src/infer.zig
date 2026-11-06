// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! ML inference runtime.
//!
//! Wraps the C `ove_model_*` API for running TFLite model inference.
//!
//! Models must be trusted, valid build-time assets aligned to 16 bytes and
//! retained immutable through close. Misaligned assets return InvalidParam and
//! schema mismatches return NotSupported. Native TFLM is unmodified. Below the startup
//! precheck minimum, setup returns OutOfMemory before SDK construction. Once
//! persistent and preparation requirements are met, exhausted allocation/planning
//! capacity returns OutOfMemory. Intermediate undersized arenas are configuration
//! errors with undefined behavior; other setup failures report their native error.
//!
//! Each admitted setup clears the effective arena in O(arena_size) time. Measure
//! arenaUsed() on the target/provider, retain at least usage plus 16 bytes and
//! qualify that configuration; final usage need not include temporary setup peaks.
//! Remeasure after model/provider/TFLM changes. Output data is read-only.
//! The C import also exposes ove_model_binding_arena (effective live span) and
//! pointer-free ove_model_binding_in_isr, independently of ASYNC. These binding
//! queries transfer no ownership and do not replace caller-side program,
//! backing or serialization obligations.

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
/// Supports float32, int8, uint8, int16 and int32 with ranks zero (scalar)
/// through five. Unused dimensions are zero; other types or higher ranks
/// return error.NotSupported from input/output. Output data is read-only.
pub const TensorInfo = struct {
    data: ?*anyopaque,
    size: usize,
    tensor_type: TensorType,
    ndims: u32,
    dims: [5]i32,
};

/// Native construction reports Busy while any session owns the shared backend,
/// including setup and final cleanup. Forgetting a session retains admission.
/// Retain model bytes/storage/arena through close returning; serialize operations.
/// Admission does not validate arbitrary model bytes or arena requirements.
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

    pub fn deinit(self: *Model) void {
        if (self.handle == null) return;
        c.ove_model_destroy(self.handle);
        self.handle = null;
    }

    /// Run the model forward pass.
    pub fn invoke(self: Model) Error!void {
        try err.fromCode(c.ove_model_invoke(self.handle));
    }

    /// Get an exact descriptor; unsupported element types or ranks above five
    /// return error.NotSupported.
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

    /// Get an exact descriptor for a read-only output; unsupported element
    /// types or ranks above five return error.NotSupported.
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

    /// Native usage including original alignment padding; zero for a closed handle.
    /// Keep at least usage plus 16 bytes and the qualified setup capacity.
    pub fn arenaUsed(self: Model) usize {
        return c.ove_model_arena_used(self.handle);
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

    /// Initialise using a caller-owned arena at least config.arena_size bytes
    /// long. Rejects insufficient extent before native effects and preserves
    /// accepted configured sizes exactly. Retain bytes, arena and this wrapper
    /// at stable addresses through close; serialize operations and reuse.
    ///
    /// A first failed init grants no usable object or cleanup rights. A failed
    /// attempt on a live wrapper preserves its existing session, including Busy.
    pub fn init(self: *Model, arena: []u8, config: *const c.ove_model_config) Error!void {
        if (config.arena_size > arena.len) return error.InvalidParam;
        try err.fromCode(c.ove_model_init(&self.handle, &self.storage, arena.ptr, config));
        self.tracker = .{};
        self.tracker.record(self);
    }

    /// Close a successfully initialized session. Repeated close is a no-op;
    /// never call on an undefined object or after a failed first initialization.
    pub fn deinit(self: *Model) void {
        if (self.handle == null) return;
        self.tracker.assertSame(self, "ove.Model");
        c.ove_model_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    /// Run the model forward pass.
    pub fn invoke(self: *Model) Error!void {
        self.tracker.assertSame(self, "ove.Model");
        try err.fromCode(c.ove_model_invoke(self.handle));
    }

    /// Get an exact descriptor; unsupported element types or ranks above five
    /// return error.NotSupported.
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

    /// Get an exact descriptor for a read-only output; unsupported element
    /// types or ranks above five return error.NotSupported.
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

    /// Native usage including original alignment padding; zero for a closed handle.
    /// Keep at least usage plus 16 bytes and the qualified setup capacity.
    pub fn arenaUsed(self: *Model) usize {
        if (self.handle == null) return 0;
        self.tracker.assertSame(self, "ove.Model");
        return c.ove_model_arena_used(self.handle);
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

/// Pointer and truthful byte extent of immutable model data.
/// This metadata does not validate linkage, storage lifetime or model programs.
pub const ModelSlice = struct {
    ptr: [*]const u8,
    len: usize,
};

/// Reference a model array and its length from `models/convert.py`.
///
/// ```zig
/// const my_model = ove.infer.linkedModel("g_my_model_data");
/// // my_model.ptr, my_model.len
/// ```
/// The named symbol must be a complete initialized immutable byte array and
/// `data_symbol ++ "_len"` its matching `unsigned int` length object. Retain
/// both for the program lifetime and establish the actual 16-byte alignment.
/// Generated objects meet this linkage contract; arbitrary definitions are not
/// validated. This returns the array address, not the value of its first word.
/// Native program, arena and numeric input qualification remain separate.
pub fn linkedModel(comptime data_symbol: []const u8) ModelSlice {
    const data = @extern([*]const u8, .{ .name = data_symbol });
    const length = @extern(*const c_uint, .{ .name = data_symbol ++ "_len" });
    return .{ .ptr = data, .len = @intCast(length.*) };
}

/// Adapt an already obtained immutable pointer and truthful byte length.
/// The caller retains the complete allocation and establishes its alignment;
/// prefer linkedModel for the generated array/length symbol pair.
pub fn modelSlice(data: [*]const u8, len: u32) ModelSlice {
    return .{ .ptr = data, .len = @intCast(len) };
}
