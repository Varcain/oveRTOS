/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file types.hpp
 * @brief Common type definitions and concepts for the C++ wrapper layer
 */

#pragma once

#include <ove/ove.h>
#include <cstdint>
#include <cstddef>
#include <concepts>
#include <type_traits>
#include <utility>

/**
 * @namespace ove
 * @brief Top-level namespace for all oveRTOS C++ abstractions.
 *
 * The `ove` namespace provides C++20 RAII wrappers around the oveRTOS C API.
 * All sync primitives, threads, queues, timers, and peripheral helpers live
 * here.  Nested namespaces (`ove::console`, `ove::time`, `ove::gpio`, etc.)
 * group thin inline wrappers around optional subsystem APIs that are enabled
 * by their corresponding `CONFIG_OVE_*` Kconfig options.
 *
 * @note All classes in this namespace are non-copyable.  Move semantics are
 *       available unless `CONFIG_OVE_ZERO_HEAP` is set, in which case the
 *       underlying kernel object lives in a member storage buffer and the
 *       address of that buffer must remain stable for the lifetime of the
 *       wrapper.
 */
namespace ove
{

/* ------------------------------------------------------------------ */
/*  C++20 Concepts                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Concept satisfied by any callable convertible to `void(*)(void*)`.
 *
 * Used to constrain the entry-function template parameter of `Thread` so
 * that only stateless function pointers or compatible functors are accepted.
 *
 * @tparam F The callable type to check.
 */
template <typename F>
concept ThreadEntry = std::convertible_to<F, void (*)(void *)>;

/* ------------------------------------------------------------------ */
/*  Compile-time C-ABI shape check                                    */
/* ------------------------------------------------------------------ */
/*
 * Pin the OVE_ERR_* numeric values at compile time.  Mirrors Rust's
 * `_assert_codes_match()` (bindings/rust/ove/src/error.rs:127-150)
 * and Zig's `comptime { std.debug.assert(...) }` block
 * (bindings/zig/ove/src/error.zig:88-102).  If a future C-header
 * rename or renumber drifts an OVE_ERR_* value, every C++ TU that
 * includes <ove/types.hpp> (i.e. every TU in apps/cpp and the C++
 * binding) fails to build with a clear message — the silent-drift
 * surface that bit Rust's mangled-name audit is closed at the C++
 * boundary too.
 */
static_assert(OVE_ERR_NOT_REGISTERED  == -1,  "OVE_ERR_NOT_REGISTERED drifted");
static_assert(OVE_ERR_INVALID_PARAM   == -2,  "OVE_ERR_INVALID_PARAM drifted");
static_assert(OVE_ERR_NO_MEMORY       == -3,  "OVE_ERR_NO_MEMORY drifted");
static_assert(OVE_ERR_TIMEOUT         == -4,  "OVE_ERR_TIMEOUT drifted");
static_assert(OVE_ERR_NOT_SUPPORTED   == -5,  "OVE_ERR_NOT_SUPPORTED drifted");
static_assert(OVE_ERR_QUEUE_FULL      == -6,  "OVE_ERR_QUEUE_FULL drifted");
static_assert(OVE_ERR_NET_REFUSED     == -8,  "OVE_ERR_NET_REFUSED drifted");
static_assert(OVE_ERR_NET_UNREACHABLE == -9,  "OVE_ERR_NET_UNREACHABLE drifted");
static_assert(OVE_ERR_NET_ADDR_IN_USE == -10, "OVE_ERR_NET_ADDR_IN_USE drifted");
static_assert(OVE_ERR_NET_RESET       == -11, "OVE_ERR_NET_RESET drifted");
static_assert(OVE_ERR_NET_DNS_FAIL    == -12, "OVE_ERR_NET_DNS_FAIL drifted");
static_assert(OVE_ERR_NET_CLOSED      == -13, "OVE_ERR_NET_CLOSED drifted");

} // namespace ove
