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

} // namespace ove
