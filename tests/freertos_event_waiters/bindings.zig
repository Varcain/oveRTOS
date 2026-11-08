// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
const std = @import("std");
const ove = @import("ove");
extern "c" fn fixture_binding_start() void;
extern "c" fn fixture_ready_count(count: c_uint) void;
extern "c" fn fixture_prepare_signal() void;
extern "c" fn fixture_await_signal() void;
extern "c" fn fixture_binding_report(language: [*:0]const u8) c_int;

fn consume(event: ove.Event) void {
    event.wait();
}
pub fn main() !void {
    fixture_binding_start();
    var bytes: [1024]u8 align(16) = undefined;
    var backing = std.heap.FixedBufferAllocator.init(&bytes);
    {
        var event = try ove.Event.create(backing.allocator());
        defer event.deinit();
        const first = try std.Thread.spawn(.{}, consume, .{event});
        errdefer {
            fixture_ready_count(1);
            fixture_prepare_signal();
            event.signal();
            fixture_await_signal();
            first.join();
        }
        const second = try std.Thread.spawn(.{}, consume, .{event});
        fixture_ready_count(2);
        for (0..2) |_| {
            fixture_prepare_signal();
            event.signal();
            fixture_await_signal();
        }
        first.join();
        second.join();
    }
    std.process.exit(@intCast(fixture_binding_report("zig")));
}
