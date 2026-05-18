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
const Thread = ove.Thread;
const Queue = ove.Queue;
const Timer = ove.Timer;

const lvgl = ove.lvgl;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const app_title = if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_FREERTOS"))
    "oveRTOS(FreeRTOS) Zig Demo"
else if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_NUTTX"))
    "oveRTOS(NuttX) Zig Demo"
else if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_ZEPHYR"))
    "oveRTOS(Zephyr) Zig Demo"
else if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_POSIX"))
    "oveRTOS(POSIX) Zig Demo"
else
    "oveRTOS Zig Demo";

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
    ove.log.inf("Producer started", .{});
    var count: u32 = 0;

    while (true) {
        count += 1;

        // In zero-heap mode the wrapper is pinned; methods take *Self,
        // so we call directly on the file-scope `queue` variable.
        queue.send(&count, 1000 * std.time.ns_per_ms) catch |e| {
            switch (e) {
                error.Timeout => ove.log.wrn("Producer: send timeout", .{}),
                error.QueueFull => ove.log.wrn("Producer: queue full, dropped {d}", .{count}),
            }
            ove.thread.sleepMs(500);
            continue;
        };

        ove.thread.sleepMs(500);
    }
}

fn consumerEntry() void {
    ove.log.inf("Consumer started", .{});

    while (true) {
        const val = queue.receive(ove.wait_forever) catch {
            ove.log.err("Consumer: receive error", .{});
            continue;
        };

        last_value.store(val, .release);

        if (val % 5 == 0) {
            ove.log.inf("Consumer: count = {d}", .{val});
        }
    }
}

// ---------------------------------------------------------------------------
// App entry — zero-heap uses two-phase init against caller-owned storage.
// ---------------------------------------------------------------------------

fn appMain() void {
    ove.log.inf("Zig example (zero-heap mode): init", .{});

    // Two-phase init: each `init()` fills in &self.storage / &self.stack
    // (which live inside the file-scope variables we declared above)
    // and registers the kernel object against those addresses.
    queue.init() catch {
        ove.log.err("Failed to init queue", .{});
        return;
    };

    graphics_thread.spawnStatic(.{ .name = "graphics", .priority = .high }, graphicsEntry, .{}) catch {
        ove.log.err("Failed to spawn graphics", .{});
        return;
    };
    producer_thread.spawnStatic(.{ .name = "producer", .priority = .normal }, producerEntry, .{}) catch {
        ove.log.err("Failed to spawn producer", .{});
        return;
    };
    consumer_thread.spawnStatic(.{ .name = "consumer", .priority = .normal }, consumerEntry, .{}) catch {
        ove.log.err("Failed to spawn consumer", .{});
        return;
    };

    ui_timer.init(uiTimerCallback, 200, .periodic) catch {
        ove.log.err("Failed to init UI timer", .{});
        return;
    };

    lvgl.init() catch {
        ove.log.err("Failed to init LVGL", .{});
        return;
    };

    {
        const guard = lvgl.lock();
        defer guard.deinit();
        createUi();
    }
    ove.log.inf("LVGL widgets created", .{});

    ui_timer.start() catch {
        ove.log.err("Failed to start UI timer", .{});
        return;
    };

    ove.log.inf("Zig example (zero-heap mode): ready", .{});

    ove.run();

    ove.log.inf("Zig example (zero-heap mode): shutdown", .{});
}

comptime {
    ove.exportMain(appMain);
}
