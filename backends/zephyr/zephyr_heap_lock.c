/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr backend extras for the portable heap-lock surface in
 * backends/common/ove_heap_lock.c.
 *
 * In zero-heap mode Zephyr is configured with HEAP_MEM_POOL_SIZE=0
 * and PICOLIBC=y — neither k_malloc nor a libc heap exists.  The
 * link-time audit (cmake/OveZeroHeapAudit.cmake) confirms this; the
 * heap-lock wrap is a defence-in-depth layer that catches any
 * regression where someone re-enables a heap.  When --wrap=malloc is
 * wired and an allocator is somehow back, calls go through the
 * wrappers in ove_heap_lock.c and trip the trap.  The weak fallbacks
 * below return NULL so any direct call to __wrap_malloc (e.g. from a
 * test harness) when --wrap isn't wired fails loudly rather than
 * recursing.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_RTOS_ZEPHYR) && defined(CONFIG_OVE_ZERO_HEAP)

#include <stddef.h>

/*
 * Weak __real_* fallbacks — same pattern as the FreeRTOS backend.
 * Forward to libc malloc when --wrap isn't wired; the linker's
 * strong __real_* alias overrides these when --wrap is in effect.
 * In Zephyr zero-heap, picolibc has no heap so malloc returns NULL,
 * and the audit catches any heap region that sneaks back in.
 *
 * memalign is intentionally NOT included.  Referencing memalign() from
 * a weak fallback pulls picolibc-module's nano-memalign.c.o into the
 * link, which transitively pulls nano-free.c.o and nano-malloc.c.o —
 * each then collides with Zephyr's COMMON_LIBC_MALLOC providing the
 * same symbols (PICOLIBC `imply`s COMMON_LIBC_MALLOC, so common is
 * always in the link too).  Tests that need to exercise the trap do
 * it via __wrap_malloc directly (test_public_create_heap_lock_traps),
 * never via memalign.  Production code paths don't call libc memalign.
 * If a future caller needs aligned allocation, prefer the explicitly-
 * sized OVE_*_DEFINE_STATIC macros.
 */
extern void *malloc(size_t);
extern void  free(void *);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);

__attribute__((weak)) void *__real_malloc(size_t n)
	{ return malloc(n); }
__attribute__((weak)) void *__real_calloc(size_t nmemb, size_t n)
	{ return calloc(nmemb, n); }
__attribute__((weak)) void *__real_realloc(void *p, size_t n)
	{ return realloc(p, n); }
__attribute__((weak)) void *__real_zalloc(size_t n)
	{ return calloc(1, n); }
__attribute__((weak)) void  __real_free(void *p)
	{ free(p); }

#endif /* CONFIG_OVE_RTOS_ZEPHYR && CONFIG_OVE_ZERO_HEAP */
