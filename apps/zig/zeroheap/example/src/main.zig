// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Zig example — zero-heap mode.
//!
//! Showcases the static-allocation Zig pattern over the same
//! producer/consumer/UI flow as the C, C++, Rust, and heap-mode Zig
//! examples.
//!
//!   - File-scope `var x: T = undefined;` declarations reserve BSS
//!     space — the wrapper struct in zero-heap mode embeds the kernel
//!     storage AND (for Thread/Workqueue) the thread stack inline as
//!     struct fields.
//!   - Two-phase init: `try x.init(...);` fills in `&self.storage` /
//!     `&self.stack` and calls `ove_*_init` against those addresses.
//!     The wrappers are pinned for the rest of their lifetime; the
//!     binding's debug-mode `Tracker` panics on any move after init().
//!   - `Type.create()` is not even declared in this build — there are
//!     no `_create()` / `_destroy()` symbols linked anywhere.
//!
//! LVGL specifics: the TLSF pool is pinned to LV_MEM_SIZE bytes in BSS
//! (CONFIG_LV_MEM_SIZE_KILOBYTES) with expansion disabled, so no
//! allocation ever falls back to the system malloc.  All widget
//! creation happens once in `createUi()` before `ove.run()`; label text
//! uses `textStatic` with caller-owned buffers — no per-update realloc.

const std = @import("std");
const ove = @import("ove");

/// Route `std.log.*` and any library using `std.log.scoped(...)` through
/// `ove.log.logFn` — emits to the oveRTOS console.
pub const std_options: std.Options = .{
    .logFn = ove.log.logFn,
};
const Thread = ove.Thread;
const Queue = ove.Queue;
const Timer = ove.Timer;

const lvgl = ove.lvgl;

// ---------------------------------------------------------------------------
// Static-backed allocator for binding primitives (zero-heap mode).
//
// Allocator-aware `create(allocator)` constructors need somewhere to put
// the substrate's storage struct.  A `FixedBufferAllocator` over a `.bss`
// byte buffer routes every allocation to caller-owned static memory —
// substrate `_init` paths see those addresses and run there.  The
// libc-malloc wrapper is never touched.
//
// Sizing: three `Thread(4096)` backings dominate.  Non-Zephyr targets
// use 8-byte stack alignment, so each backing is ~4 KB stack + ~120 B
// storage = ~4.2 KB.  Zephyr requires power-of-2 stack alignment
// matching `stack_size + 128`, so each backing is 8 KB on ARM Zephyr.
// 32 KB covers the worst case (3 × 8 KB) plus queue/timer storage with
// headroom — well within the SRAM budget of the canonical zero-heap
// targets (STM32F746 has 320 KB; Zephyr nucleo-h743 has 1 MB).
// ---------------------------------------------------------------------------
var arena_bytes: [32 * 1024]u8 = undefined;
var fba: std.heap.FixedBufferAllocator = undefined;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const app_title = switch (ove.target.current_rtos) {
    .freertos => "oveRTOS(FreeRTOS) Zig Demo",
    .nuttx => "oveRTOS(NuttX) Zig Demo",
    .zephyr => "oveRTOS(Zephyr) Zig Demo",
    .posix => "oveRTOS(POSIX) Zig Demo",
    .wasm => "oveRTOS(wasm) Zig Demo",
};

// ---------------------------------------------------------------------------
// Shared state — wrappers embed kernel storage and (for Thread) the
// stack as struct fields.  Each `var x: T = undefined;` reserves the
// full sizeof(T) in BSS; init() populates the bytes in place.
// ---------------------------------------------------------------------------

var queue: Queue(u32, 8) = undefined;
var ui_timer: Timer = undefined;
var graphics_thread: Thread(4096) = undefined;
var producer_thread: Thread(4096) = undefined;
var consumer_thread: Thread(4096) = undefined;

var last_value: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);

var counter_label: ?lvgl.Label = null;
var bar: ?lvgl.Bar = null;

// Counter label text buffer — caller-owned, pinned via `textStatic` so
// LVGL stores the pointer rather than copying into its pool.  Updated
// in place by the timer callback under the LVGL lock.
var count_buf: [24:0]u8 = std.mem.zeroes([24:0]u8);

// ---------------------------------------------------------------------------
// LVGL UI
// ---------------------------------------------------------------------------

fn createUi() void {
    const screen = lvgl.screenActive();

    // Set background to black (must not chain — ABI issues with 3-byte
    // lv_color_t return through Zig on ARM)
    ove.ffi.lv_obj_set_style_bg_color(screen.obj, ove.ffi.lv_color_black(), 0);
    ove.ffi.lv_obj_set_style_bg_opa(screen.obj, 255, 0);

    _ = lvgl.Label.create(screen)
        .textStatic(app_title)
        .font(lvgl.fontMontserrat32())
        .color(lvgl.colorWhite())
        .alignTo(ove.ffi.LV_ALIGN_TOP_MID, 0, 16);

    // Pin the counter label to our caller-owned buffer.  After
    // createUi() the timer callback only rewrites `count_buf` in
    // place — the label's text pointer stays valid and LVGL just
    // re-renders without allocating.
    @memcpy(count_buf[0..8], "Count: 0");
    counter_label = lvgl.Label.create(screen)
        .textStatic(&count_buf)
        .font(lvgl.fontMontserrat14())
        .color(lvgl.colorWhite())
        .alignTo(ove.ffi.LV_ALIGN_TOP_MID, 0, 64);

    bar = lvgl.Bar.create(screen)
        .size(200, 16)
        .range(0, 100)
        .value(0)
        .indicatorColor(lvgl.paletteMain(ove.ffi.LV_PALETTE_BLUE))
        .radius(8)
        .alignTo(ove.ffi.LV_ALIGN_TOP_MID, 0, 96);
}

fn uiTimerCallback() void {
    const val = last_value.load(.acquire);

    const guard = lvgl.lock();
    defer guard.deinit();

    // Refresh count_buf in place; LVGL stored its address once at
    // createUi() time via `textStatic` — `textStatic` again with the
    // same pointer just triggers a redraw, no allocation.
    for (count_buf[0..]) |*b| b.* = 0;
    _ = std.fmt.bufPrintZ(&count_buf, "Count: {d}", .{val}) catch return;
    if (counter_label) |*label| _ = label.textStatic(&count_buf);
    if (bar) |*b| _ = b.value(@intCast(val % 101));
}

// ---------------------------------------------------------------------------
// Thread entries
// ---------------------------------------------------------------------------

fn graphicsEntry() void {
    var last_us: u64 = ove.time.getUs() catch 0;

    while (true) {
        const now_us: u64 = ove.time.getUs() catch last_us;
        const elapsed_ms: u32 = @intCast((now_us - last_us) / 1000);
        last_us = now_us;

        {
            const guard = lvgl.lock();
            defer guard.deinit();
            lvgl.tick(elapsed_ms);
            lvgl.handler();
        }
        ove.thread.sleepMs(33);
    }
}

fn producerEntry() void {
    std.log.info("Producer started", .{});
    var count: u32 = 0;

    while (true) {
        count += 1;

        // In zero-heap mode the wrapper is pinned; methods take *Self,
        // so we call directly on the file-scope `queue` variable.
        queue.sendFor(&count, .millis(1000)) catch |e| {
            switch (e) {
                error.Timeout => std.log.warn("Producer: send timeout", .{}),
                error.QueueFull => std.log.warn("Producer: queue full, dropped {d}", .{count}),
            }
            ove.thread.sleepMs(500);
            continue;
        };

        ove.thread.sleepMs(500);
    }
}

fn consumerEntry() void {
    std.log.info("Consumer started", .{});

    while (true) {
        const val = queue.recv();

        last_value.store(val, .release);

        if (val % 5 == 0) {
            std.log.info("Consumer: count = {d}", .{val});
        }
    }
}

// ---------------------------------------------------------------------------
// App entry — zero-heap uses two-phase init against caller-owned storage.
// ---------------------------------------------------------------------------

fn appMain() void {
    std.log.info("Zig example (zero-heap mode): init", .{});

    // Zero-heap allocator setup: FixedBufferAllocator over a static
    // BSS buffer.  Allocator-aware `create()` calls draw from this
    // pool; substrate `_init` paths see those addresses.
    fba = std.heap.FixedBufferAllocator.init(&arena_bytes);
    const allocator = fba.allocator();

    queue = Queue(u32, 8).create(allocator) catch {
        std.log.err("Failed to create queue", .{});
        return;
    };

    graphics_thread = ove.Thread(4096).spawn(allocator, .{ .name = "graphics", .priority = .high }, graphicsEntry, .{}) catch {
        std.log.err("Failed to spawn graphics", .{});
        return;
    };
    producer_thread = ove.Thread(4096).spawn(allocator, .{ .name = "producer", .priority = .normal }, producerEntry, .{}) catch {
        std.log.err("Failed to spawn producer", .{});
        return;
    };
    consumer_thread = ove.Thread(4096).spawn(allocator, .{ .name = "consumer", .priority = .normal }, consumerEntry, .{}) catch {
        std.log.err("Failed to spawn consumer", .{});
        return;
    };

    ui_timer = Timer.create(allocator, .{ .period_ms = 200, .mode = .periodic }, uiTimerCallback, .{}) catch {
        std.log.err("Failed to init UI timer", .{});
        return;
    };

    lvgl.init() catch {
        std.log.err("Failed to init LVGL", .{});
        return;
    };

    {
        const guard = lvgl.lock();
        defer guard.deinit();
        createUi();
    }
    std.log.info("LVGL widgets created", .{});

    ui_timer.start() catch {
        std.log.err("Failed to start UI timer", .{});
        return;
    };

    std.log.info("Zig example (zero-heap mode): ready", .{});

    ove.run();

    std.log.info("Zig example (zero-heap mode): shutdown", .{});
}

comptime {
    ove.exportMain(appMain);
}
