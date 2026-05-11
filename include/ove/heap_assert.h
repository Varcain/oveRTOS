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
 * When `CONFIG_OVE_ZERO_HEAP=y`, calls to libc malloc / calloc /
 * realloc / zalloc / memalign from oveRTOS application code fail at
 * compile time with a `__attribute__((error))` diagnostic.  This is a
 * hard layer beneath the runtime trap in
 * `backends/common/ove_heap_lock.c` — most violations are caught
 * before the binary is even linked.
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

/* bindgen (libclang) rejects the `__attribute__((error(...)))`
 * redeclarations below as "attribute does not appear on the first
 * declaration" against the libc decls.  GCC accepts that pattern; the
 * Rust crate's build.rs passes -D__BINDGEN__, so skip the redecls there
 * — bindgen never emits libc allocator symbols anyway.
 *
 * Emscripten/clang has the same strict behaviour as bindgen's libclang:
 * the `error` attribute can only appear on the first declaration of a
 * function, not a redeclaration.  Skip the trap there too — the WASM
 * sim build doesn't have a kernel heap to protect anyway, and the
 * runtime trap in ove_heap_lock.c remains in place for any backend
 * that does.
 *
 * Cross-compile clang-tidy (config/ove-cli/ove/lint.py:_clang_tidy_backends)
 * shares clang's strictness: it parses with --target=arm-none-eabi and
 * trips on the same redeclaration pattern.  The lint script defines
 * `__OVE_LINT__` so we can opt out — lint never produces a binary, so
 * the compile-time guard isn't doing anything useful in that pass. */
#if defined(CONFIG_OVE_ZERO_HEAP) && !defined(__BINDGEN__) && !defined(__ZIG_CIMPORT__) && \
	!defined(__EMSCRIPTEN__) && !defined(__OVE_LINT__)

#include <stddef.h>

#define _OVE_HEAP_FORBIDDEN(name)                                                        \
	__attribute__((error("oveRTOS zero-heap mode forbids libc " name "(); use "      \
			     "OVE_*_DEFINE_STATIC / ove_*_init() with caller-supplied "  \
			     "storage, or build with CONFIG_OVE_ZERO_HEAP=n if dynamic " \
			     "allocation is required.")))

/* C++ libc declarations carry `noexcept`; redeclaring without it is
 * a hard error.  C has no exception specifiers, so the macro expands
 * to nothing there. */
#ifdef __cplusplus
#define _OVE_HEAP_NOTHROW noexcept
#else
#define _OVE_HEAP_NOTHROW
#endif

/*
 * Redeclare libc allocators with the `error` attribute.  GCC + Clang
 * accept redeclarations that add attributes; any subsequent call site
 * that sees this declaration fails compilation with the message
 * above.
 */
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

#endif /* CONFIG_OVE_ZERO_HEAP */

#endif /* OVE_HEAP_ASSERT_H */
