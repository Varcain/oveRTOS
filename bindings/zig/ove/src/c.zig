// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Raw C FFI — `@cImport("ove/ove.h")` exposing every public C symbol.
//!
//! `__ZIG_CIMPORT__` is defined so the headers surface opaque storage types;
//! Zig only uses pointer handles, never the struct internals. This avoids
//! pulling in complex RTOS headers (`zephyr/kernel.h`, `nuttx/config.h`) that
//! `@cImport` can't parse. Higher-level modules build on top and never
//! re-export `raw` directly to app code.

// Raw C FFI — single @cImport for oveRTOS + LVGL.
// __ZIG_CIMPORT__ gives opaque storage types (avoids complex RTOS headers).
// LVGL headers are included when CONFIG_OVE_LVGL is set in ove_config.h.
//
// Raw C FFI — single @cImport for oveRTOS + LVGL.
// __ZIG_CIMPORT__ gives opaque storage types (avoids complex RTOS headers).
// LVGL headers are included when CONFIG_OVE_LVGL is set in ove_config.h.
//
// CMake may inject `wint_t=unsigned int` at the -D level for cross builds
// where the sysroot's `sys/_types.h` references `wint_t` without including
// its definition (newlib + Zig 0.16 translate-c regression — Zig 0.15
// papered over this via an implicit libc).  See config/cmake/ove_zig.cmake.
pub const raw = @cImport({
    @cDefine("__ZIG_CIMPORT__", "1");
    @cInclude("ove/ove.h");
    @cInclude("ove/lvgl.h");
});
