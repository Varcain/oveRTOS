/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file ove/heap_assert.h
 * @brief Compile-time guard against libc heap allocators in zero-heap mode.
 *
 * When `CONFIG_OVE_ZERO_HEAP=y`, this guard diagnoses libc allocator
 * calls at compile time using GCC's `error` attribute or Clang's
 * `unavailable` attribute.  It complements the runtime trap in
 * `backends/common/ove_heap_lock.c`.
 *
 * Pulled in via `ove/ove.h`, so any TU that includes the umbrella
 * header is covered.  Backend infra that legitimately needs the libc
 * allocator path (e.g. the wrap trampolines in
 * `backends/common/ove_heap_lock.c` calling `__real_malloc`, or the
 * NuttX backend's `mm_malloc(USR_HEAP, ...)` direct calls) bypasses
 * this header by not including `ove/ove.h` from the path that touches
 * those symbols, or by referencing the renamed/internal symbols
 * directly.
 *
 * `free()` is intentionally not poisoned: destruction is allowed
 * post-lock (see the rationale in `ove_heap_lock.c:__wrap_free`),
 * and `free(NULL)` is a common cleanup idiom.
 */

#ifndef OVE_HEAP_ASSERT_H
#define OVE_HEAP_ASSERT_H

#include "ove_config.h"

/* Bindgen, Zig @cImport, and clang-tidy parse this header without building
 * a binary, so skip the allocator guard for these tools. */
#if defined(CONFIG_OVE_ZERO_HEAP) && !defined(__BINDGEN__) && !defined(__ZIG_CIMPORT__) && \
	!defined(__OVE_LINT__)

#include <stddef.h>

#define _OVE_HEAP_FORBIDDEN_MSG(name)                               \
	"oveRTOS zero-heap mode forbids libc " name "(); use "      \
	"OVE_*_DEFINE_STATIC / ove_*_init() with caller-supplied "  \
	"storage, or build with CONFIG_OVE_ZERO_HEAP=n if dynamic " \
	"allocation is required."

#ifdef __clang__
/* Clang C++ misses std:: names imported by <cstdlib> before this header.
 * GCC (used by all product builds) also rejects calls through those names. */
#define _OVE_HEAP_FORBIDDEN(name) __attribute__((unavailable(_OVE_HEAP_FORBIDDEN_MSG(name))))
#else
#define _OVE_HEAP_FORBIDDEN(name) __attribute__((error(_OVE_HEAP_FORBIDDEN_MSG(name))))
#endif

/* C++ libc declarations carry `noexcept`; redeclaring without it is
 * a hard error.  C has no exception specifiers, so the macro expands
 * to nothing there. */
#ifdef __cplusplus
#define _OVE_HEAP_NOTHROW noexcept
#else
#define _OVE_HEAP_NOTHROW
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern void *malloc(size_t) _OVE_HEAP_NOTHROW _OVE_HEAP_FORBIDDEN("malloc");
extern void *calloc(size_t, size_t) _OVE_HEAP_NOTHROW _OVE_HEAP_FORBIDDEN("calloc");
extern void *realloc(void *, size_t) _OVE_HEAP_NOTHROW _OVE_HEAP_FORBIDDEN("realloc");
extern void *zalloc(size_t) _OVE_HEAP_NOTHROW _OVE_HEAP_FORBIDDEN("zalloc");
extern void *memalign(size_t, size_t) _OVE_HEAP_NOTHROW _OVE_HEAP_FORBIDDEN("memalign");
extern void *aligned_alloc(size_t, size_t) _OVE_HEAP_NOTHROW _OVE_HEAP_FORBIDDEN("aligned_alloc");

#ifdef __cplusplus
}
#endif

#undef _OVE_HEAP_NOTHROW
#undef _OVE_HEAP_FORBIDDEN
#undef _OVE_HEAP_FORBIDDEN_MSG

#endif /* CONFIG_OVE_ZERO_HEAP */

#endif /* OVE_HEAP_ASSERT_H */
