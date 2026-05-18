// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Live DMIC Keyword Detection — Zig implementation
//!
//! Uses the comptime `addProcessor` wrapper for the audio node and the
//! `AudioBuf` safe wrapper — zero `@ptrCast` or `callconv(.c)` in this
//! file.  All FFI trampolines are generated at comptime inside the
//! `ove.audio` binding layer.

const std = @import("std");
const ove = @import("ove");

// FixedBufferAllocator over a static BSS buffer — zero-heap-compatible.
var arena_bytes: [8192]u8 = undefined;
var fba: std.heap.FixedBufferAllocator = undefined;


/// Route `std.log.*` and any library using `std.log.scoped(...)` through
/// `ove.log.logFn` — emits to the oveRTOS console.
pub const std_options: std.Options = .{
    .logFn = ove.log.logFn,
};
const infer = ove.infer;

// ── Constants ──────────────────────────────────────────────────────────

const feature_size: usize = 40;
const feature_count: usize = 49;
const feature_elements: usize = feature_size * feature_count;
const audio_sample_freq: u32 = 16000;
const feature_stride_ms: u32 = 20;
const feature_duration_ms: u32 = 30;
const audio_duration_samples: usize = feature_duration_ms * audio_sample_freq / 1000;
const category_count: usize = 4;
const arena_size: usize = 32768;
const confidence_threshold: f32 = 0.6;
const noise_gate_threshold: i32 = 500;
const target_peak: i32 = 15000;
const ring_capacity: usize = 32768;
const ring_mask: usize = ring_capacity - 1;
// I2S slot handling is now done by the driver — app sees clean PCM.

const labels = [_][]const u8{ "silence", "unknown", "yes", "no" };

// ── Model data (linked from C objects) ─────────────────────────────────

extern const g_audio_preprocessor_int8_model_data: [*]const u8;
extern const g_audio_preprocessor_int8_model_data_len: u32;
extern const g_micro_speech_quantized_model_data: [*]const u8;
extern const g_micro_speech_quantized_model_data_len: u32;

fn preprocessorModel() infer.ModelSlice {
    return infer.modelSlice(
        g_audio_preprocessor_int8_model_data,
        g_audio_preprocessor_int8_model_data_len,
    );
}

fn classifierModel() infer.ModelSlice {
    return infer.modelSlice(
        g_micro_speech_quantized_model_data,
        g_micro_speech_quantized_model_data_len,
    );
}

// ── Lock-free SPSC ring buffer ────────────────────────────────────────

const RingBuffer = struct {
    data: [ring_capacity]i16 = [_]i16{0} ** ring_capacity,
    head: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),
    tail: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),

    /// Write a sample (single-producer side — audio callback).
    fn write(self: *RingBuffer, sample: i16) void {
        const h = self.head.load(.monotonic);
        self.data[h & ring_mask] = sample;
        _ = self.head.fetchAdd(1, .release);
    }

    fn available(self: *const RingBuffer) u32 {
        return self.head.load(.acquire) -| self.tail.load(.monotonic);
    }

    /// Read the most recent samples into `out` (single-consumer side).
    fn readLast(self: *RingBuffer, out: []i16) void {
        const count = out.len;
        const h = self.head.load(.acquire);
        const start: usize = if (h >= count) h - count else 0;
        for (0..count) |i| {
            out[i] = self.data[(start + i) & ring_mask];
        }
        self.tail.store(h, .monotonic);
    }
};

// ── Shared state ───────────────────────────────────────────────────────
//
// Only audio_ring and samples_written are shared between threads.
// All other mutable state is local to inferThread.

var audio_ring: RingBuffer = .{};
var samples_written: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);

// ── DMIC processor node ────────────────────────────────────────────────

const DmicProcessor = struct {
    pub fn process(self: *DmicProcessor, input: ove.audio.AudioBuf, output: ove.audio.AudioBuf) void {
        _ = self;
        const src = input.dataS16();
        const dst = output.dataS16Mut();
        const frames = input.frames();
        const ch = input.channels();

        for (0..frames) |f| {
            const sample = src[f * ch]; // left / mono
            audio_ring.write(sample);
            _ = samples_written.fetchAdd(1, .monotonic);

            // Passthrough to output for monitoring
            for (0..ch) |c| {
                dst[f * ch + c] = src[f * ch + c];
            }
        }
    }
};

var dmic_proc: DmicProcessor = .{};

// Audio graph + inference thread — zero-heap mode embeds kernel
// storage and stack inline as struct members.
var graph: ove.audio.Graph = undefined;
var infer_thread: ove.Thread(8192) = undefined;

// ── DSP parameters ────────────────────────────────────────────────────

const DspState = struct {
    actual_rate: u32 = audio_sample_freq,
    dc_offset: i32 = 0,
    gain: i32 = 1,
};

// ── Feature extraction ─────────────────────────────────────────────────

fn generateFeatures(
    audio: []const i16,
    features: *[feature_count][feature_size]i8,
    storage: *[arena_size]u8,
    dsp: DspState,
) !void {
    const slice = preprocessorModel();
    const cfg = ove.ffi.ove_model_config{
        .model_data = slice.ptr,
        .model_size = slice.len,
        .arena_size = arena_size,
    };
    var preproc: ove.Model = undefined;
    try preproc.init(storage, &cfg);
    defer preproc.deinit();

    const actual_window = feature_duration_ms * dsp.actual_rate / 1000;
    const actual_stride = feature_stride_ms * dsp.actual_rate / 1000;

    var frame: usize = 0;
    var offset: usize = 0;
    while (offset + actual_window <= audio.len and frame < feature_count) {
        const input = try preproc.inputData(i16, 0);

        for (0..audio_duration_samples) |i| {
            const src_idx = offset + (i * dsp.actual_rate / audio_sample_freq);
            var s: i32 = if (src_idx < audio.len) @as(i32, audio[src_idx]) else 0;
            s -= dsp.dc_offset;
            s = std.math.clamp(s * dsp.gain, -32768, 32767);
            input[i] = @intCast(s);
        }

        try preproc.invoke();

        const output = try preproc.outputData(i8, 0);
        @memcpy(&features[frame], output[0..feature_size]);

        frame += 1;
        offset += actual_stride;
    }
}

// ── Classification ─────────────────────────────────────────────────────

const Prediction = struct {
    index: usize,
    confidence: f32,
};

fn classifyKeyword(
    features: *const [feature_count][feature_size]i8,
    storage: *[arena_size]u8,
) !Prediction {
    const slice = classifierModel();
    const cfg = ove.ffi.ove_model_config{
        .model_data = slice.ptr,
        .model_size = slice.len,
        .arena_size = arena_size,
    };
    var classifier: ove.Model = undefined;
    try classifier.init(storage, &cfg);
    defer classifier.deinit();

    const input = try classifier.inputData(u8, 0);
    for (0..feature_count) |row| {
        const src_bytes = std.mem.asBytes(&features[row]);
        @memcpy(input[row * feature_size ..][0..feature_size], src_bytes);
    }

    try classifier.invoke();

    const scores = try classifier.outputData(i8, 0);
    var best: usize = 0;
    for (1..category_count) |i| {
        if (scores[i] > scores[best]) best = i;
    }

    return .{
        .index = best,
        .confidence = (@as(f32, @floatFromInt(scores[best])) + 128.0) / 255.0,
    };
}

// ── Inference thread ───────────────────────────────────────────────────

fn inferThread() void {
    std.log.info("Inference thread started — listening...", .{});
    ove.thread.sleepMs(2000);
    var prev_samples = samples_written.load(.monotonic);

    // Thread-local state
    var audio_window = [_]i16{0} ** audio_sample_freq;
    var features = std.mem.zeroes([feature_count][feature_size]i8);
    var storage: [arena_size]u8 align(16) = undefined;
    var dsp = DspState{};

    while (true) {
        ove.thread.sleepMs(1000);

        const cur = samples_written.load(.monotonic);
        const actual_rate = cur -| prev_samples;
        prev_samples = cur;

        const read_count: usize = if (actual_rate > 0)
            @min(actual_rate, audio_sample_freq)
        else
            audio_sample_freq;

        if (audio_ring.available() < read_count) continue;
        audio_ring.readLast(audio_window[0..read_count]);

        // Peak detection
        var peak: i16 = 0;
        for (audio_window[0..read_count]) |s| {
            const a: i16 = @intCast(@abs(s));
            if (a > peak) peak = a;
        }
        std.log.info("Audio: peak={d}, rate={d}, read={d}", .{ peak, actual_rate, read_count });

        dsp.actual_rate = if (actual_rate > 0) actual_rate else audio_sample_freq;
        if (peak < 10) {
            std.log.warn("Audio silent — check DMIC", .{});
            continue;
        }

        // DC offset
        var sum: i64 = 0;
        for (audio_window[0..read_count]) |s| sum += s;
        dsp.dc_offset = @intCast(@divTrunc(sum, @as(i64, @intCast(read_count))));

        // Noise gate + adaptive gain
        var dc_peak: i32 = 0;
        for (audio_window[0..read_count]) |s| {
            const d: i32 = @intCast(@abs(@as(i32, s) - dsp.dc_offset));
            if (d > dc_peak) dc_peak = d;
        }
        if (dc_peak < noise_gate_threshold) continue;

        dsp.gain = std.math.clamp(@divTrunc(target_peak, dc_peak), 1, 200);
        std.log.info("  dc_peak={d}, gain={d}", .{ dc_peak, dsp.gain });

        // Inference pipeline
        generateFeatures(audio_window[0..read_count], &features, &storage, dsp) catch {
            std.log.err("Features failed", .{});
            continue;
        };

        if (classifyKeyword(&features, &storage)) |result| {
            if (result.index > 1 and result.confidence > confidence_threshold) {
                std.log.info(">>> Keyword: \"{s}\" ({d:.0}%)", .{
                    labels[result.index],
                    result.confidence * 100.0,
                });
            }
        } else |_| {}
    }
}

// ── Entry point ────────────────────────────────────────────────────────

fn appMain() void {
    fba = std.heap.FixedBufferAllocator.init(&arena_bytes);
    const allocator = fba.allocator();

    std.log.info("=== Live DMIC Keyword Detection (Zig) ===", .{});
    std.log.info("Models: preprocessor {d} + classifier {d} bytes", .{
        preprocessorModel().len,
        classifierModel().len,
    });

    graph.init(512) catch {
        std.log.err("Audio graph init failed", .{});
        ove.run();
        return;
    };

    const dev_cfg = ove.audio.Graph.deviceCfgI2s(16000, 1, 1);

    const src = graph.deviceSource(&dev_cfg, "dmic-in") catch {
        std.log.err("Failed to create DMIC source", .{});
        ove.run();
        return;
    };
    const proc = graph.addProcessor(DmicProcessor, &dmic_proc, "dmic-proc") catch {
        std.log.err("Failed to add processor", .{});
        ove.run();
        return;
    };
    const sink = graph.deviceSink(&dev_cfg, "hp-out") catch {
        std.log.err("Failed to create HP sink", .{});
        ove.run();
        return;
    };

    graph.connect(@intCast(src), @intCast(proc)) catch {
        std.log.err("Graph connect failed", .{});
        ove.run();
        return;
    };
    graph.connect(@intCast(proc), @intCast(sink)) catch {
        std.log.err("Graph connect failed", .{});
        ove.run();
        return;
    };
    graph.build() catch {
        std.log.err("Graph build failed", .{});
        ove.run();
        return;
    };
    graph.start() catch {
        std.log.err("Graph start failed", .{});
        ove.run();
        return;
    };
    std.log.info("Audio streaming: 16kHz mono, DMIC input", .{});

    infer_thread = ove.Thread(8192).spawn(allocator, .{ .name = "infer", .priority = .normal }, inferThread, .{}) catch {
        std.log.err("Failed to init infer thread", .{});
        return;
    };

    std.log.info("Say \"yes\" or \"no\" near the microphone...", .{});
    ove.run();
}

comptime {
    ove.exportMain(appMain);
}
