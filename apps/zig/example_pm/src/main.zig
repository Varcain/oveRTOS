// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Zig Power Management Example
//!
//! Demonstrates the ove.pm module:
//!   - Sleep state machine with auto-idle
//!   - Peripheral power domain reference counting
//!   - Wake source registration (GPIO + UART)
//!   - Runtime power statistics reporting

const std = @import("std");
const ove = @import("ove");
const Thread = ove.Thread;
const prio = ove.thread.prio;

const pm = ove.pm;

// ---------------------------------------------------------------------------
// Simulated battery level
// ---------------------------------------------------------------------------

var battery_pct: std.atomic.Value(i32) = std.atomic.Value(i32).init(85);

// ---------------------------------------------------------------------------
// Sensor thread: periodic read with domain management
// ---------------------------------------------------------------------------

fn sensorEntry() void {
    var reading: u32 = 0;

    ove.log.inf("sensor: started", .{});

    while (true) {
        pm.domainRequest(1) catch {}; // SENSOR = 1
        pm.activity();

        Thread.sleepMs(50);
        reading +%= 17;
        ove.log.inf("sensor: reading = {d}", .{reading % 1000});

        pm.domainRelease(1) catch {}; // SENSOR = 1

        Thread.sleepMs(5000);
    }
}

// ---------------------------------------------------------------------------
// Monitor thread: print power statistics
// ---------------------------------------------------------------------------

fn monitorEntry() void {
    ove.log.inf("monitor: started", .{});

    while (true) {
        Thread.sleepMs(10000);

        if (pm.getStats()) |stats| {
            ove.log.inf("=== Power Stats ===", .{});
            ove.log.inf("  active:  {d} us ({d} trans)", .{
                stats.time_in_state_us[0],
                stats.transition_count[0],
            });
            ove.log.inf("  idle:    {d} us ({d} trans)", .{
                stats.time_in_state_us[1],
                stats.transition_count[1],
            });
            ove.log.inf("  standby: {d} us ({d} trans)", .{
                stats.time_in_state_us[2],
                stats.transition_count[2],
            });
            ove.log.inf("  active%: {d}.{d:0>2}%", .{
                stats.active_pct_x100 / 100,
                stats.active_pct_x100 % 100,
            });
        } else |_| {}

        const batt = battery_pct.load(.monotonic);
        if (batt > 5) {
            battery_pct.store(batt - 5, .monotonic);
        }
        ove.log.inf("battery: {d}%", .{batt});
    }
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn appMain() void {
    ove.log.inf("pm example (Zig): init", .{});

    pm.init(.{
        .idle_threshold_ms = 50,
        .standby_threshold_ms = 5000,
        .deep_sleep_threshold_ms = 30000,
    }) catch |e| {
        ove.log.err("PM init failed: {}", .{e});
        return;
    };

    // Register wake sources
    pm.wakeRegisterGpio(0, 13, 0x02) catch {}; // falling edge
    pm.wakeRegisterUart(0) catch {};

    // Set power budget target: 60% low-power
    pm.setBudget(6000) catch {};

    // Create threads
    _ = Thread.spawn("sensor", sensorEntry, prio.normal, 4096) catch {
        ove.log.err("Failed to create sensor thread", .{});
        return;
    };
    _ = Thread.spawn("monitor", monitorEntry, prio.low, 4096) catch {
        ove.log.err("Failed to create monitor thread", .{});
        return;
    };

    ove.log.inf("pm example (Zig): ready (battery={d}%)", .{
        battery_pct.load(.monotonic),
    });

    ove.run();

    pm.deinit();

    ove.log.inf("pm example (Zig): shutdown", .{});
}

comptime {
    ove.exportMain(appMain);
}
