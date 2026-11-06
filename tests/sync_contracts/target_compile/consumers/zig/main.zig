// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
const std = @import("std");
const ove = @import("ove");
// New test backing only. No existing application/model arena changes.
var bytes: [8192]u8 align(64) = undefined;

fn constructors() !void {
    var fba = std.heap.FixedBufferAllocator.init(&bytes);
    const allocator = fba.allocator();
    var mutex = try ove.Mutex.create(allocator);
    defer mutex.deinit();
    var recursive = try ove.RecursiveMutex.create(allocator);
    defer recursive.deinit();
    var semaphore = try ove.Semaphore.create(allocator, 0, 2);
    defer semaphore.deinit();
    var event = try ove.Event.create(allocator);
    defer event.deinit();
    var cv = try ove.CondVar.create(allocator);
    defer cv.deinit();
}

export fn sync_binding_compile_consumer() c_int {
    constructors() catch return 1;
    return 0;
}

fn appMain() void {
    ove.run();
}
comptime {
    ove.exportMain(appMain);
}
