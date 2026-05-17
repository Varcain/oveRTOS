// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Zig example — heap mode.
//!
//! Showcases idiomatic heap-mode Zig over the same producer/consumer/UI
//! flow as the C, C++, and Rust examples:
//!
//!   - Value-returning `Type.create(...)` constructors.  In heap mode
//!     each Zig wrapper is a single handle; `.create()` calls
//!     `ove_*_create` under the hood and returns the wrapper by value.
//!     The wrappers are fully movable — no pin tracker.
//!   - File-scope handles are stored as nullable optionals
//!     (`?Type = null`) so thread entry functions (which can't capture
//!     closures) can reach them.  We `.?` to unwrap once `appMain`
//!     has populated them.
//!   - No `OVE_*_DEFINE_STATIC` macros, no embedded storage, no
//!     two-phase init.  Every kernel handle is heap-allocated.
//!
//! Pair with `apps/zig/zeroheap/example/` which uses two-phase init
//! against caller-owned embedded storage and stacks.
//!
//! LVGL operates from its own builtin TLSF pool (LV_MEM_SIZE).  In heap
//! mode the pool is allowed to grow via LV_MEM_POOL_EXPAND_SIZE.

const std = @import("std");
const ove = @import("ove");
const Thread = ove.Thread;
const Queue = ove.Queue;
const Timer = ove.Timer;
const prio = ove.thread.prio;

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
// Shared state — heap mode keeps everything optional and populated at
// runtime.  The wrapper types are 1-pointer handles so this layout is
// genuinely cheap.
// ---------------------------------------------------------------------------

var queue: ?Queue(u32, 8) = null;
var ui_timer: ?Timer = null;
var graphics_thread: ?Thread(4096) = null;
var producer_thread: ?Thread(4096) = null;
var consumer_thread: ?Thread(4096) = null;

var last_value: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);

var counter_label: ?lvgl.Label = null;
var bar: ?lvgl.Bar = null;

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
        .text(app_title)
        .font(lvgl.fontMontserrat32())
        .color(lvgl.colorWhite())
        .alignTo(ove.ffi.LV_ALIGN_TOP_MID, 0, 16);

    counter_label = lvgl.Label.create(screen)
        .text("Count: 0")
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

    // Heap-mode label text: lv_label_set_text duplicates the buffer
    // into LVGL's pool on every call.  We can format on the stack and
    // let LVGL own the copy.
    var buf: [24]u8 = undefined;
    _ = std.fmt.bufPrint(&buf, "Count: {d}\x00", .{val}) catch |e| {
        ove.log.err("ui timer fmt: {}", .{e});
        return;
    };
    if (counter_label) |*label| _ = label.text(@ptrCast(&buf));
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

        // `queue.?` unwraps the optional — `appMain` populated it
        // before this thread was spawned, so it is guaranteed non-null
        // here.
        queue.?.send(&count, 1000 * std.time.ns_per_ms) catch |e| {
            switch (e) {
                error.Timeout => ove.log.wrn("Producer: send timeout", .{}),
                error.QueueFull => ove.log.wrn("Producer: queue full, dropped {d}", .{count}),
                else => ove.log.err("Producer: unexpected send error", .{}),
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
        const val = queue.?.receive(ove.wait_forever) catch {
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
// App entry — heap mode allocates each handle via Type.create().
// ---------------------------------------------------------------------------

fn appMain() void {
    ove.log.inf("Zig example (heap mode): init", .{});

    queue = Queue(u32, 8).create() catch {
        ove.log.err("Failed to create queue", .{});
        return;
    };

    graphics_thread = Thread(4096).spawn(.{ .name = "graphics", .priority = prio.high }, graphicsEntry, .{}) catch {
        ove.log.err("Failed to spawn graphics", .{});
        return;
    };
    producer_thread = Thread(4096).spawn(.{ .name = "producer", .priority = prio.normal }, producerEntry, .{}) catch {
        ove.log.err("Failed to spawn producer", .{});
        return;
    };
    consumer_thread = Thread(4096).spawn(.{ .name = "consumer", .priority = prio.normal }, consumerEntry, .{}) catch {
        ove.log.err("Failed to spawn consumer", .{});
        return;
    };

    ui_timer = Timer.create(uiTimerCallback, 200, .periodic) catch {
        ove.log.err("Failed to create UI timer", .{});
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

    ui_timer.?.start() catch {
        ove.log.err("Failed to start UI timer", .{});
        return;
    };

    ove.log.inf("Zig example (heap mode): ready", .{});

    ove.run();

    ove.log.inf("Zig example (heap mode): shutdown", .{});
}

comptime {
    ove.exportMain(appMain);
}
