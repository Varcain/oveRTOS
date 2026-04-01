// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Live DMIC Keyword Detection — Zig implementation
//!
//! Uses the comptime `addProcessor` wrapper for the audio node — zero
//! `callconv(.c)` in this file.  All FFI trampolines are generated at
//! comptime inside the `ove.audio` binding layer.

const std = @import("std");
const ove = @import("ove");
const Thread = ove.Thread;
const c = ove.ffi;
const infer = ove.infer;

// ── Constants ──────────────────────────────────────────────────────────

const FEATURE_SIZE: usize = 40;
const FEATURE_COUNT: usize = 49;
const FEATURE_ELEMENTS: usize = FEATURE_SIZE * FEATURE_COUNT;
const AUDIO_SAMPLE_FREQ: u32 = 16000;
const FEATURE_STRIDE_MS: u32 = 20;
const FEATURE_DURATION_MS: u32 = 30;
const AUDIO_DURATION_SAMPLES: usize = FEATURE_DURATION_MS * AUDIO_SAMPLE_FREQ / 1000;
const CATEGORY_COUNT: usize = 4;
const ARENA_SIZE: usize = 32768;
const CONFIDENCE_THRESHOLD: f32 = 0.6;
const NOISE_GATE_THRESHOLD: i32 = 500;
const TARGET_PEAK: i32 = 15000;
const RING_BUF_CAPACITY: usize = 32768;
const RING_BUF_MASK: usize = RING_BUF_CAPACITY - 1;

const labels = [_][]const u8{ "silence", "unknown", "yes", "no" };

// ── Model data (linked from C objects) ─────────────────────────────────

extern const g_audio_preprocessor_int8_model_data: [*]const u8;
extern const g_audio_preprocessor_int8_model_data_len: u32;
extern const g_micro_speech_quantized_model_data: [*]const u8;
extern const g_micro_speech_quantized_model_data_len: u32;

fn preprocessorModel() infer.ModelSlice {
    return infer.modelSlice(g_audio_preprocessor_int8_model_data, g_audio_preprocessor_int8_model_data_len);
}
fn classifierModel() infer.ModelSlice {
    return infer.modelSlice(g_micro_speech_quantized_model_data, g_micro_speech_quantized_model_data_len);
}

// ── Lock-free ring buffer ──────────────────────────────────────────────

const RingBuffer = struct {
    data: [RING_BUF_CAPACITY]i16 = [_]i16{0} ** RING_BUF_CAPACITY,
    head: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),
    tail: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),

    fn write(self: *RingBuffer, sample: i16) void {
        const h = self.head.load(.monotonic);
        self.data[h & RING_BUF_MASK] = sample;
        _ = self.head.fetchAdd(1, .release);
    }

    fn available(self: *const RingBuffer) u32 {
        return self.head.load(.acquire) -| self.tail.load(.monotonic);
    }

    fn readLast(self: *RingBuffer, out: []i16, count: usize) void {
        const h = self.head.load(.acquire);
        const start: usize = if (h >= count) h - count else 0;
        for (0..count) |i| {
            out[i] = self.data[(start + i) & RING_BUF_MASK];
        }
        self.tail.store(h, .monotonic);
    }
};

// ── Shared state ───────────────────────────────────────────────────────

var audio_ring: RingBuffer = .{};
var samples_written: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);
var features: [FEATURE_COUNT][FEATURE_SIZE]i8 = std.mem.zeroes([FEATURE_COUNT][FEATURE_SIZE]i8);
var g_actual_rate: u32 = AUDIO_SAMPLE_FREQ;
var g_dc_offset: i32 = 0;
var g_gain: i32 = 1;

var arena: [ARENA_SIZE]u8 align(16) = std.mem.zeroes([ARENA_SIZE]u8);
var model_storage: c.ove_model_storage_t = std.mem.zeroes(c.ove_model_storage_t);

// ── DMIC processor node ────────────────────────────────────────────────
// Uses Graph.addProcessor — zero callconv(.c) here.

const DmicProcessor = struct {
    pub fn process(self: *DmicProcessor, in_buf: [*c]const c.struct_ove_audio_buf, out_buf: [*c]c.struct_ove_audio_buf) void {
        _ = self;
        const src: [*]const i16 = @ptrCast(@alignCast(in_buf.*.data));
        const dst: [*]i16 = @ptrCast(@alignCast(out_buf.*.data));
        const num_frames = in_buf.*.frames / 4;

        for (0..num_frames) |f| {
            const base = f * 4;
            const mic_l = src[base + 1];
            audio_ring.write(mic_l);
            _ = samples_written.fetchAdd(1, .monotonic);

            dst[base + 0] = mic_l;
            dst[base + 1] = 0;
            dst[base + 2] = src[base + 3];
            dst[base + 3] = 0;
        }
    }
};

var dmic_proc: DmicProcessor = .{};

// ── Feature extraction ─────────────────────────────────────────────────

fn generateFeatures(audio: []const i16) i32 {
    const cfg = c.struct_ove_model_config{
        .model_data = preprocessorModel().ptr,
        .model_size = preprocessorModel().len,
        .arena_size = ARENA_SIZE,
    };
    var preproc = infer.Model.initWithArena(&model_storage, &arena, &cfg) catch return -1;
    defer preproc.destroy();

    const input_info = preproc.input(0) catch return -1;
    const output_info = preproc.output(0) catch return -1;

    const actual_window = FEATURE_DURATION_MS * g_actual_rate / 1000;
    const actual_stride = FEATURE_STRIDE_MS * g_actual_rate / 1000;

    var frame: usize = 0;
    var offset: usize = 0;
    while (offset + actual_window <= audio.len and frame < FEATURE_COUNT) {
        const input: [*]i16 = @ptrCast(@alignCast(input_info.data));

        for (0..AUDIO_DURATION_SAMPLES) |i| {
            const src_idx = offset + (i * g_actual_rate / AUDIO_SAMPLE_FREQ);
            var s: i32 = if (src_idx < audio.len) @as(i32, audio[src_idx]) else 0;
            s -= g_dc_offset;
            s = @as(i32, @intCast(std.math.clamp(s * g_gain, -32768, 32767)));
            input[i] = @intCast(s);
        }

        preproc.invoke() catch return -1;

        const output: [*]const i8 = @ptrCast(output_info.data);
        @memcpy(&features[frame], output[0..FEATURE_SIZE]);

        frame += 1;
        offset += actual_stride;
    }
    return 0;
}

// ── Classification ─────────────────────────────────────────────────────

fn classifyKeyword() ?struct { prediction: usize, confidence: f32 } {
    const cfg = c.struct_ove_model_config{
        .model_data = classifierModel().ptr,
        .model_size = classifierModel().len,
        .arena_size = ARENA_SIZE,
    };
    var classifier = infer.Model.initWithArena(&model_storage, &arena, &cfg) catch return null;
    defer classifier.destroy();

    const input_info = classifier.input(0) catch return null;
    const output_info = classifier.output(0) catch return null;

    const input: [*]u8 = @ptrCast(input_info.data);
    const feat_bytes: [*]const u8 = @ptrCast(&features);
    @memcpy(input[0..FEATURE_ELEMENTS], feat_bytes[0..FEATURE_ELEMENTS]);

    classifier.invoke() catch return null;

    const scores: [*]const i8 = @ptrCast(output_info.data);
    var best: usize = 0;
    for (1..CATEGORY_COUNT) |i| {
        if (scores[i] > scores[best]) best = i;
    }

    const confidence = (@as(f32, @floatFromInt(scores[best])) + 128.0) / 255.0;
    return .{ .prediction = best, .confidence = confidence };
}

// ── Inference thread ───────────────────────────────────────────────────

var audio_window: [AUDIO_SAMPLE_FREQ]i16 = std.mem.zeroes([AUDIO_SAMPLE_FREQ]i16);

fn inferThread() void {
    ove.log.inf("Inference thread started", .{});
    Thread.sleepMs(2000);
    var prev_samples = samples_written.load(.monotonic);

    while (true) {
        Thread.sleepMs(1000);

        const cur = samples_written.load(.monotonic);
        const actual_rate = cur -| prev_samples;
        prev_samples = cur;

        const read_count: usize = if (actual_rate > 0)
            @min(actual_rate, AUDIO_SAMPLE_FREQ)
        else
            AUDIO_SAMPLE_FREQ;

        if (audio_ring.available() < read_count) continue;
        audio_ring.readLast(&audio_window, read_count);

        // Peak
        var peak: i16 = 0;
        for (0..read_count) |i| {
            const s = if (audio_window[i] < 0) -audio_window[i] else audio_window[i];
            if (s > peak) peak = s;
        }
        ove.log.inf("Audio: peak={d}, rate={d}, read={d}", .{ peak, actual_rate, read_count });

        g_actual_rate = if (actual_rate > 0) actual_rate else AUDIO_SAMPLE_FREQ;
        if (peak < 10) { ove.log.wrn("Audio silent", .{}); continue; }

        // DC offset
        var sum: i64 = 0;
        for (0..read_count) |i| sum += audio_window[i];
        g_dc_offset = @intCast(@divTrunc(sum, @as(i64, @intCast(read_count))));

        // Noise gate
        var dc_peak: i32 = 0;
        for (0..read_count) |i| {
            var s: i32 = @as(i32, audio_window[i]) - g_dc_offset;
            if (s < 0) s = -s;
            if (s > dc_peak) dc_peak = s;
        }
        if (dc_peak < NOISE_GATE_THRESHOLD) continue;

        g_gain = std.math.clamp(@divTrunc(TARGET_PEAK, dc_peak), 1, 200);
        ove.log.inf("  dc_peak={d}, gain={d}", .{ dc_peak, g_gain });

        // Inference
        if (generateFeatures(audio_window[0..read_count]) != 0) {
            ove.log.err("Features failed", .{});
            continue;
        }

        if (classifyKeyword()) |result| {
            if (result.prediction > 1 and result.confidence > CONFIDENCE_THRESHOLD) {
                ove.log.inf(">>> Keyword: \"{s}\" ({d:.0}%)", .{
                    labels[result.prediction],
                    result.confidence * 100.0,
                });
            }
        }
    }
}

// ── Entry point ────────────────────────────────────────────────────────

fn appMain() void {
    ove.log.inf("=== Live DMIC Keyword Detection (Zig) ===", .{});
    ove.log.inf("Models: preprocessor {d} + classifier {d} bytes", .{ preprocessorModel().len, classifierModel().len });

    // Audio graph
    var graph = ove.audio.Graph.init(512) catch {
        ove.log.err("Audio graph init failed", .{});
        ove.run();
        return;
    };

    const dev_cfg = ove.audio.Graph.deviceCfgI2s(16000, 1, 1);

    const src = graph.deviceSource(&dev_cfg, "dmic-in") catch -1;
    const proc_idx = graph.addProcessor(DmicProcessor, &dmic_proc, "dmic-proc") catch -1;
    const sink = graph.deviceSink(&dev_cfg, "hp-out") catch -1;

    if (src < 0 or proc_idx < 0 or sink < 0) {
        ove.log.err("Audio node creation failed", .{});
        ove.run();
        return;
    }

    graph.connect(@intCast(src), @intCast(proc_idx)) catch {};
    graph.connect(@intCast(proc_idx), @intCast(sink)) catch {};
    graph.build() catch {};
    graph.start() catch {};
    ove.log.inf("Audio streaming: 16kHz mono", .{});

    _ = Thread.spawn("infer", inferThread, ove.thread.prio.normal, 8192) catch {
        ove.log.err("Failed to spawn infer thread", .{});
        return;
    };

    ove.log.inf("Say \"yes\" or \"no\" near the microphone...", .{});
    ove.run();
}

comptime {
    ove.exportMain(appMain);
}
