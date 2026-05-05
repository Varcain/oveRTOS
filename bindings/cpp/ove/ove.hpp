/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file ove.hpp
 * @brief Umbrella header — includes all oveRTOS C++ wrappers
 *
 * @section discipline Hot-path inline discipline
 *
 * Every wrapper in this binding lives in a `.hpp` (header-only).  The
 * compiler sees through to the underlying `ove_*(...)` C call without
 * any cross-TU function-call boundary, so the LLVM optimizer folds
 * the wrapper into the caller's frame natively — no
 * `[[gnu::always_inline]]` annotation is required to reach the
 * zero-cost-abstraction baseline.  STM32F746 wrapper-Δ rows verified
 * at 65–500 ns over the raw native RTOS API, matching the Zig binding
 * (which also relies on whole-program LLVM inlining without explicit
 * `inline fn` markers in most places).
 *
 * Discipline rules for any new wrapper added here:
 *   - Define the wrapper body in this `.hpp` (no separate `.cpp` TU
 *     for the hot path).
 *   - Mark dtors and move operations `noexcept`.  No `throw` anywhere.
 *   - Mark error-returning calls `[[nodiscard]]`.  Error codes flow as
 *     raw `int` (negative on error); the C++ binding does not wrap them
 *     into typed errors because in-language interop with C is the
 *     point.  Compare with the Rust+Zig bindings, which carry typed
 *     errors because they cross an FFI boundary.
 *
 * @section build Build profile
 *
 * The firmware-side build (via `cmake/OveCommon.cmake`) compiles C++
 * with `-fno-exceptions -fno-rtti` by default — gated by the CMake
 * option `OVE_CXX_NOEXCEPT_NORTTI=ON`.  This pulls `.eh_frame` /
 * `.gcc_except_table` / typeinfo vtables out of the firmware ELF for
 * a meaningful binary-size shrink.  Disable via
 * `-DOVE_CXX_NOEXCEPT_NORTTI=OFF` if a downstream project legitimately
 * needs C++ exceptions or RTTI.
 *
 * Cross-language LTO (between C, C++, Rust, and Zig) is gated behind
 * `OVE_CROSS_LTO=ON` (also declared in `cmake/OveCommon.cmake`).  Per
 * Gale's "three quiet barriers" analysis the win on already-fast hot
 * paths is small (1–4 cycles per cross-call) and the failure mode
 * silent — turn it on only after measuring the FFI hop dominates a
 * real workload.
 *
 * @section compile_check Compile-time C-ABI shape check
 *
 * `<ove/types.hpp>` carries a `static_assert` block validating the
 * numeric values of every `OVE_ERR_*` constant.  This catches silent
 * drift if a future C-header rename or renumber breaks the contract,
 * mirroring `_assert_codes_match()` in the Rust binding and the
 * `comptime { std.debug.assert(...) }` block in the Zig binding.
 */

#pragma once

#include <ove/types.hpp>
#include <ove/sync.hpp>
#include <ove/eventgroup.hpp>
#include <ove/queue.hpp>
#include <ove/timer.hpp>
#include <ove/thread.hpp>
#include <ove/console.hpp>
#include <ove/time.hpp>
#include <ove/nvs.hpp>
#include <ove/shell.hpp>
#include <ove/board.hpp>
#include <ove/gpio.hpp>
#include <ove/led.hpp>
#include <ove/bsp.hpp>
#include <ove/audio.hpp>
#include <ove/watchdog.hpp>
#include <ove/fs.hpp>
#include <ove/stream.hpp>
#include <ove/workqueue.hpp>
#include <ove/app.hpp>
#include <ove/net.hpp>
#include <ove/net_tls.hpp>
#include <ove/net_http.hpp>
#include <ove/net_mqtt.hpp>
#include <ove/net_httpd.hpp>
#include <ove/pm.hpp>

/* Zephyr's <zephyr/kernel_structs.h> defines `_current` and `_kernel`
 * as global object-like macros (`#define _current _kernel.cpus[0].current`).
 * Both leak into every TU that pulls in any Zephyr header (i.e. anything
 * including ove.hpp on Zephyr).  Third-party C++ libraries — notably
 * ETL's <etl/ranges.h>, used by lvgl_gallery_cpp — declare members
 * named `_current`, which the preprocessor then mangles into
 * `_kernel.cpus[0].current` before C++ parses, producing a torrent of
 * "expected unqualified-id" errors.
 *
 * Undef both AT THE END of the umbrella header.  oveRTOS's C++ bindings
 * never call into raw Zephyr scheduler internals from app code; the
 * affected macros are only used inside Zephyr's own static-inline
 * accessors, which have already been parsed by this point. */
#ifdef __ZEPHYR__
#undef _current
#undef _kernel
#endif
