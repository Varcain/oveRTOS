// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
const std = @import("std");
const ove = @import("ove");

var bytes: [2048]u8 align(64) = undefined;

fn eventBindingTaskConsumer() callconv(.c) c_int {
    var fba = std.heap.FixedBufferAllocator.init(&bytes);
    var event = ove.Event.create(fba.allocator()) catch return 1;
    defer event.deinit();
    event.signal();
    event.timedWait(.millis(1)) catch return 1;
    return 0;
}

fn eventBindingIsrConsumer(event: *const ove.Event) callconv(.c) void {
    event.signalFromIsr();
}

export const event_compile_task_anchor: *const fn () callconv(.c) c_int = eventBindingTaskConsumer;
export const event_compile_isr_anchor: *const fn (*const ove.Event) callconv(.c) void = eventBindingIsrConsumer;

fn appMain() void {
    ove.run();
}
comptime {
    ove.exportMain(appMain);
}
