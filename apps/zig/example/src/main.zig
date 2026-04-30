// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

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
// Shared state
// ---------------------------------------------------------------------------

var queue: Queue(u32, 8) = undefined;
var last_value: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);

var counter_label: ?lvgl.Label = null;
var bar: ?lvgl.Bar = null;
var ui_timer: Timer = undefined;

var graphics_thread: Thread(4096) = undefined;
var producer_thread: Thread(4096) = undefined;
var consumer_thread: Thread(4096) = undefined;

// ---------------------------------------------------------------------------
// LVGL UI
// ---------------------------------------------------------------------------

fn createUi() void {
    const screen = lvgl.screenActive();

    // Set background to black (must not chain — ABI issues with 3-byte
    // lv_color_t return through Zig on ARM)
    ove.ffi.lv_obj_set_style_bg_color(screen.obj, ove.ffi.lv_color_black(), 0);
    ove.ffi.lv_obj_set_style_bg_opa(screen.obj, 255, 0);

    // Title
    _ = lvgl.Label.create(screen)
        .text(app_title)
        .font(lvgl.fontMontserrat32())
        .color(lvgl.colorWhite())
        .alignTo(ove.ffi.LV_ALIGN_TOP_MID, 0, 16);

    // Counter label
    counter_label = lvgl.Label.create(screen)
        .text("Count: 0")
        .font(lvgl.fontMontserrat14())
        .color(lvgl.colorWhite())
        .alignTo(ove.ffi.LV_ALIGN_TOP_MID, 0, 64);

    // Progress bar
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

        queue.send(&count, 1000) catch |e| {
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
// App entry
// ---------------------------------------------------------------------------

fn appMain() void {
    ove.log.inf("Zig example: init", .{});

    // Create queue
    queue.init() catch {
        ove.log.err("Failed to create queue", .{});
        return;
    };

    // Create threads
    graphics_thread.init("graphics", graphicsEntry, prio.high) catch {
        ove.log.err("Failed to spawn graphics", .{});
        return;
    };
    producer_thread.init("producer", producerEntry, prio.normal) catch {
        ove.log.err("Failed to spawn producer", .{});
        return;
    };
    consumer_thread.init("consumer", consumerEntry, prio.normal) catch {
        ove.log.err("Failed to spawn consumer", .{});
        return;
    };

    // Initialize LVGL and create UI
    ui_timer.init(uiTimerCallback, 200, .periodic) catch {
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

    ui_timer.start() catch {
        ove.log.err("Failed to start UI timer", .{});
        return;
    };

    ove.log.inf("Zig example: ready", .{});

    ove.run();

    ove.log.inf("Zig example: shutdown", .{});
}

comptime {
    ove.exportMain(appMain);
}
