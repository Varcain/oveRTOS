// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// Raw C FFI — @cImport("ove/ove.h")
//
// Use __ZIG_CIMPORT__ to get opaque storage types — Zig only uses pointer
// handles, never the struct internals. This avoids pulling in complex
// RTOS headers (zephyr/kernel.h, nuttx/config.h) that @cImport can't parse.

// Raw C FFI — single @cImport for oveRTOS + LVGL.
// __ZIG_CIMPORT__ gives opaque storage types (avoids complex RTOS headers).
// LVGL headers are included when CONFIG_OVE_LVGL is set in ove_config.h.
pub const raw = @cImport({
    @cDefine("__ZIG_CIMPORT__", "1");
    @cInclude("ove/ove.h");
    @cInclude("ove/lvgl.h");
});
