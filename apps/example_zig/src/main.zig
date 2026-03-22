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

const has_lvgl = @hasDecl(ove.ffi, "ove_lvgl_init");
const lvgl = if (has_lvgl) ove.lvgl else undefined;

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
var last_value: u32 = 0;

var counter_label: if (has_lvgl) lvgl.Label else void = if (has_lvgl) undefined else {};
var bar: if (has_lvgl) lvgl.Bar else void = if (has_lvgl) undefined else {};
var ui_timer: Timer = undefined;

// ---------------------------------------------------------------------------
// LVGL UI
// ---------------------------------------------------------------------------

fn createUi() void {
    if (!has_lvgl) return;

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
    if (!has_lvgl) return;

    const val = last_value;

    const guard = lvgl.lock();
    defer guard.deinit();

    var buf: [24]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, "Count: {d}\x00", .{val}) catch return;
    _ = text;
    _ = counter_label.text(@ptrCast(&buf));
    _ = bar.value(@intCast(val % 101));
}

// ---------------------------------------------------------------------------
// Thread entries
// ---------------------------------------------------------------------------

fn graphicsEntry() void {
    while (true) {
        {
            const guard = lvgl.lock();
            lvgl.tick(33);
            lvgl.handler();
            guard.deinit();
        }
        Thread.sleepMs(33);
    }
}

fn producerEntry() void {
    ove.console.write("[I] Producer started\n");
    var count: u32 = 0;

    while (true) {
        count += 1;

        queue.send(&count, 1000) catch |e| {
            switch (e) {
                error.Timeout => ove.console.write("[W] Producer: send timeout\n"),
                error.QueueFull => ove.print("[W] Producer: queue full, dropped {d}\n", .{count}),
                else => ove.console.write("[E] Producer: unexpected send error\n"),
            }
            Thread.sleepMs(500);
            continue;
        };

        Thread.sleepMs(500);
    }
}

fn consumerEntry() void {
    ove.console.write("[I] Consumer started\n");

    while (true) {
        const val = queue.receive(ove.wait_forever) catch {
            ove.console.write("[E] Consumer: receive error\n");
            continue;
        };

        last_value = val;

        if (val % 5 == 0) {
            ove.print("[I] Consumer: count = {d}\n", .{val});
        }
    }
}

// ---------------------------------------------------------------------------
// App entry
// ---------------------------------------------------------------------------

fn appMain() void {
    ove.console.write("[I] Zig example: init\n");

    // Create queue
    queue = Queue(u32, 8).create() catch {
        ove.console.write("[E] Failed to create queue\n");
        return;
    };

    // Create threads
    if (has_lvgl) {
        _ = Thread.spawn("graphics", graphicsEntry, prio.high, 4096) catch {
            ove.console.write("[E] Failed to spawn graphics\n");
            return;
        };
    }

    _ = Thread.spawn("producer", producerEntry, prio.normal, 4096) catch {
        ove.console.write("[E] Failed to spawn producer\n");
        return;
    };

    _ = Thread.spawn("consumer", consumerEntry, prio.normal, 4096) catch {
        ove.console.write("[E] Failed to spawn consumer\n");
        return;
    };

    // Initialize LVGL and create UI
    if (has_lvgl) {
        ui_timer = Timer.create(uiTimerCallback, 200, false) catch {
            ove.console.write("[E] Failed to create UI timer\n");
            return;
        };

        lvgl.init() catch {
            ove.console.write("[E] Failed to init LVGL\n");
            return;
        };

        {
            const guard = lvgl.lock();
            createUi();
            guard.deinit();
        }
        ove.console.write("[I] LVGL widgets created\n");

        ui_timer.start() catch {
            ove.console.write("[E] Failed to start UI timer\n");
            return;
        };
    }

    ove.console.write("[I] Zig example: ready\n");

    ove.run();

    ove.console.write("[I] Zig example: shutdown\n");
}

comptime {
    ove.exportMain(appMain);
}
