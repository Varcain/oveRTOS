// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Zig networking example — zero-heap mode (placeholder).
//!
//! The full networking test suite uses runtime allocation paths inside
//! the lwIP / TLS / HTTP / MQTT clients that cannot be expressed
//! statically.  See apps/zig/heap/example_net/ for the full demo.

const ove = @import("ove");
const Thread = ove.Thread;
const prio = ove.thread.prio;

var net_thread: Thread(4096) = undefined;

fn netStub() void {
    ove.log.inf("net (zero-heap): see apps/zig/heap/example_net for the full demo", .{});
    while (true) ove.thread.sleepMs(10000);
}

fn appMain() void {
    ove.log.inf("Zig networking example (zero-heap mode): stub", .{});
    net_thread.init("net-stub", netStub, prio.normal) catch |e| {
        ove.log.err("Failed to init net stub thread: {}", .{e});
        return;
    };
    ove.run();
}

comptime {
    ove.exportMain(appMain);
}
