// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
const ove = @import("ove");
const infer = ove.infer;
extern fn infer_fixture_reset() void;
extern fn infer_fixture_check(condition: bool, expression: [*:0]const u8) void;
extern fn infer_fixture_finish(name: [*:0]const u8) u8;
extern fn infer_asset_pointer(which: c_uint) ?*const anyopaque;
extern fn infer_asset_length(which: c_uint) usize;
extern fn infer_asset_byte(which: c_uint, index: usize) u8;

comptime {
    if (@sizeOf(c_uint) != 4) @compileError("model length ABI requires four bytes");
    if (!@typeInfo(@FieldType(infer.ModelSlice, "ptr")).pointer.is_const)
        @compileError("model assets must remain read-only");
}

fn checkAsset(comptime symbol: []const u8, which: c_uint) void {
    const model = infer.linkedModel(symbol);
    const pointer = infer_asset_pointer(which) orelse {
        infer_fixture_check(false, "independent allocation exists");
        return;
    };
    const length = infer_asset_length(which);
    // Guard every allocation fact before constructing any bounded slice. A
    // changed metadata length remains an ordinary failure with no bad access.
    if (@intFromPtr(model.ptr) != @intFromPtr(pointer) or model.len != length or
        @intFromPtr(model.ptr) % 16 != 0)
    {
        infer_fixture_check(false, "linked span matches complete aligned allocation");
        return;
    }
    for (model.ptr[0..model.len], 0..) |byte, index| {
        infer_fixture_check(byte == infer_asset_byte(which, index), "complete payload byte");
    }
}

pub fn main() u8 {
    infer_fixture_reset();
    checkAsset("g_audio_preprocessor_int8_model_data", 0);
    checkAsset("g_micro_speech_quantized_model_data", 1);
    checkAsset("infer_tiny_model_data", 2);
    return infer_fixture_finish("assets");
}
