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

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    // Add C include paths
    if (ove_include_paths) |paths| {
        var it = std.mem.splitScalar(u8, paths, ';');
        while (it.next()) |p| {
            if (p.len > 0) {
                root_module.addIncludePath(.{ .cwd_relative = p });
            }
        }
    }

    // Add C defines
    if (ove_defines) |defs| {
        var it = std.mem.splitScalar(u8, defs, ';');
        while (it.next()) |d| {
            if (d.len == 0) continue;
            if (std.mem.indexOfScalar(u8, d, '=')) |eq| {
                root_module.addCMacro(d[0..eq], d[eq + 1 ..]);
            } else {
                root_module.addCMacro(d, "");
            }
        }
    }

    const lib = b.addLibrary(.{
        .name = "ove_zig",
        .root_module = root_module,
        .linkage = .static,
    });

    b.installArtifact(lib);

    // `zig build docs` — emit autodoc HTML under zig-out/docs/.
    // Mirrors the Makefile docs target (which currently shells out to
    // `zig build-lib --femit-docs` directly, bypassing this build.zig).
    const docs_obj = b.addObject(.{
        .name = "ove_docs",
        .root_module = root_module,
    });
    const install_docs = b.addInstallDirectory(.{
        .source_dir = docs_obj.getEmittedDocs(),
        .install_dir = .prefix,
        .install_subdir = "docs",
    });
    const docs_step = b.step("docs", "Emit autodoc HTML under zig-out/docs/");
    docs_step.dependOn(&install_docs.step);
}
