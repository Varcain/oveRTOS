// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
const std = @import("std");
const ove = @import("ove");
extern "c" fn eventgroup_test_clocks() c_uint;
pub fn main() !void {
    var group = try ove.EventGroup.create(std.heap.page_allocator);
    defer group.deinit();
    try std.testing.expectEqual(@as(u32, 0xffffffff), ove.EventGroup.validMask());
    try std.testing.expectEqual(@as(u32, 0xffffffff), group.setBits(0xffffffff));
    try std.testing.expectEqual(@as(u32, 0xffffffff), group.clearBits(1));
    try std.testing.expectEqual(@as(u32, 0xfffffffe), group.getBits());
    try std.testing.expectEqual(@as(u32, 0xfffffffe), group.clearBits(ove.EventGroup.validMask()));
    try std.testing.expectEqual(@as(u32, 0), group.setBits(0));
    try std.testing.expectEqual(@as(u32, 0), group.clearBits(0));
    try std.testing.expectError(error.InvalidParam, group.waitBitsFor(0, 0, ove.Duration.nanos(0)));
    try std.testing.expectError(error.InvalidParam, group.waitBitsFor(1, 4, ove.Duration.nanos(0)));
    const deadline = ove.Instant.now();
    const before = eventgroup_test_clocks();
    try std.testing.expectError(error.InvalidParam, group.waitBitsUntil(0, 0, deadline));
    try std.testing.expectEqual(before, eventgroup_test_clocks());
    try std.testing.expectError(error.Timeout, group.waitBitsFor(1, 0, ove.Duration.nanos(0)));
    _ = std.c.printf("eventgroup zig: checks=12 failures=0\n");
}
