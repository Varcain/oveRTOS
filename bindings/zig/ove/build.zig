// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Collect include paths from environment (set by ove_zig.cmake)
    const ove_include_paths = b.option(
        []const u8,
        "ove-include-paths",
        "Semicolon-separated list of include paths",
    );
    const ove_defines = b.option(
        []const u8,
        "ove-defines",
        "Semicolon-separated list of -Dkey=value defines",
    );

    const lib = b.addStaticLibrary(.{
        .name = "ove_zig",
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    // Add C include paths
    if (ove_include_paths) |paths| {
        var it = std.mem.splitScalar(u8, paths, ';');
        while (it.next()) |p| {
            if (p.len > 0) {
                lib.addIncludePath(.{ .cwd_relative = p });
            }
        }
    }

    // Add C defines
    if (ove_defines) |defs| {
        var it = std.mem.splitScalar(u8, defs, ';');
        while (it.next()) |d| {
            if (d.len == 0) continue;
            if (std.mem.indexOfScalar(u8, d, '=')) |eq| {
                lib.defineCMacro(d[0..eq], d[eq + 1 ..]);
            } else {
                lib.defineCMacro(d, null);
            }
        }
    }

    b.installArtifact(lib);
}
