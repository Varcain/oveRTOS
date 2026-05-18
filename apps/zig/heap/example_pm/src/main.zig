// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Zig Power Management Example — heap mode.
//!
//! Threads use the value-returning `Thread(N).spawn(...)` constructor
//! and live in `?Thread(N) = null` slots populated inside appMain.
//! Pair with apps/zig/zeroheap/example_pm/ which uses two-phase init
//! against embedded storage.

const std = @import("std");
const ove = @import("ove");

/// Allocator backing every binding primitive in this app.
const app_allocator = std.heap.page_allocator;

/// Route `std.log.*` and any library using `std.log.scoped(...)` through
/// `ove.log.logFn` — emits to the oveRTOS console.
pub const std_options: std.Options = .{
    .logFn = ove.log.logFn,
};
const Thread = ove.Thread;

const pm = ove.pm;

var battery_pct: std.atomic.Value(i32) = std.atomic.Value(i32).init(85);

var sensor_thread: ?Thread(4096) = null;
var monitor_thread: ?Thread(4096) = null;

fn batteryPolicy(
    current: pm.State,
    idle_ms: u32,
    next_timeout_ms: u32,
    user_data: ?*anyopaque,
) callconv(.c) pm.State {
    _ = current;
    _ = next_timeout_ms;
    _ = user_data;

    const batt = battery_pct.load(.monotonic);

    if (batt < 15) {
        if (idle_ms > 5) return ove.ffi.OVE_PM_STATE_DEEP_SLEEP;
        return ove.ffi.OVE_PM_STATE_STANDBY;
    }

    if (idle_ms < 10) return ove.ffi.OVE_PM_STATE_ACTIVE;
    if (idle_ms < 1000) return ove.ffi.OVE_PM_STATE_IDLE;
    if (idle_ms < 10000) return ove.ffi.OVE_PM_STATE_STANDBY;
    return ove.ffi.OVE_PM_STATE_DEEP_SLEEP;
}

fn pmNotify(event: pm.Event, from: pm.State, to: pm.State) void {
    if (event == ove.ffi.OVE_PM_EVENT_PRE_SLEEP) {
        std.log.info("pm: preparing sleep {d} -> {d}", .{ from, to });
    } else {
        std.log.info("pm: woke {d} -> {d}", .{ from, to });
    }
}

fn sensorEntry() void {
    std.log.info("sensor: started", .{});
    var reading: u32 = 0;
    while (true) {
        pm.domainRequest(ove.ffi.OVE_PM_DOMAIN_SENSOR) catch {};
        pm.activity();

        ove.thread.sleepMs(50);
        reading +%= 17;
        std.log.info("sensor: reading = {d}", .{reading % 1000});

        pm.domainRelease(ove.ffi.OVE_PM_DOMAIN_SENSOR) catch {};
        ove.thread.sleepMs(5000);
    }
}

fn monitorEntry() void {
    std.log.info("monitor: started", .{});
    while (true) {
        ove.thread.sleepMs(10000);

        if (pm.getStats()) |stats| {
            std.log.info("=== Power Stats ===", .{});
            std.log.info("  active:  {d} us ({d} transitions)", .{
                stats.time_in_state_us[0],
                stats.transition_count[0],
            });
            std.log.info("  idle:    {d} us ({d} transitions)", .{
                stats.time_in_state_us[1],
                stats.transition_count[1],
            });
            std.log.info("  standby: {d} us ({d} transitions)", .{
                stats.time_in_state_us[2],
                stats.transition_count[2],
            });
            std.log.info("  deep:    {d} us ({d} transitions)", .{
                stats.time_in_state_us[3],
                stats.transition_count[3],
            });
            std.log.info("  active%: {d}.{d:0>2}%", .{
                stats.active_pct_x100 / 100,
                stats.active_pct_x100 % 100,
            });
        } else |_| {}

        const cur = battery_pct.load(.monotonic);
        const next = if (cur > 5) cur - 5 else cur;
        battery_pct.store(next, .monotonic);
        std.log.info("battery: {d}%", .{next});
    }
}

fn appMain() void {
    std.log.info("pm example (heap mode): init", .{});

    pm.init(.{
        .idle_threshold_ms = 50,
        .standby_threshold_ms = 5000,
        .deep_sleep_threshold_ms = 30000,
    }) catch |e| {
        std.log.err("PM init failed: {}", .{e});
        return;
    };

    pm.wakeRegisterGpio(0, 13, ove.ffi.OVE_GPIO_IRQ_FALLING) catch {};
    pm.wakeRegisterUart(0) catch {};
    pm.notifyRegister(pmNotify) catch {};
    pm.setPolicy(batteryPolicy, null) catch {};
    pm.setBudget(6000) catch {};

    sensor_thread = Thread(4096).spawn(app_allocator, .{ .name = "sensor", .priority = .normal }, sensorEntry, .{}) catch {
        std.log.err("Failed to spawn sensor", .{});
        return;
    };
    monitor_thread = Thread(4096).spawn(app_allocator, .{ .name = "monitor", .priority = .low }, monitorEntry, .{}) catch {
        std.log.err("Failed to spawn monitor", .{});
        return;
    };

    std.log.info("pm example (heap mode): ready (battery={d}%)", .{battery_pct.load(.monotonic)});

    ove.run();

    std.log.info("pm example (heap mode): shutdown", .{});
}

comptime {
    ove.exportMain(appMain);
}
