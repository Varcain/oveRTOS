/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * NuttX backend extras for the portable heap-lock surface in
 * backends/common/ove_heap_lock.c:
 *
 *   - Weak __real_<libc-fn> fallbacks that forward to mm_malloc /
 *     mm_free / etc. directly, bypassing libc.  These resolve the link
 *     when --wrap=malloc isn't wired (e.g. NuttX flat-build paths
 *     where app-level LDFLAGS don't reach the kernel link); when
 *     --wrap IS wired the linker-generated strong __real_* aliases
 *     take precedence and these are unused.  Going via libc malloc()
 *     would loop infinitely under --wrap.
 *
 *   - __wrap_kmm_<fn> for NuttX's kernel-mm allocator surface.  In
 *     flat builds kmm_malloc is a #define for malloc, so these only
 *     apply to split-mode (CONFIG_MM_KERNEL_HEAP) builds.  Provided
 *     unconditionally so the symbol exists either way.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_RTOS_NUTTX

#include <stddef.h>

#ifdef CONFIG_OVE_ZERO_HEAP

#include <nuttx/mm/mm.h>

/* Internal trap helper + post-lock fallback hooks from
 * backends/common/ove_heap_lock.c. The fallbacks have weak NULL
 * defaults; nuttx_zh_reserved_heap.c provides strong overrides that
 * route post-lock allocations into a pre-allocated private mm_heap. */
int ove_heap_lock_trapped_(void);
void *ove_heap_lock_post_alloc_(size_t n);
void *ove_heap_lock_post_zalloc_(size_t n);
void *ove_heap_lock_post_calloc_(size_t nmemb, size_t n);
void *ove_heap_lock_post_realloc_(void *p, size_t n);
void *ove_heap_lock_post_memalign_(size_t alignment, size_t size);
int ove_heap_lock_post_free_(void *p);

/* ── Weak __real_* fallbacks for libc malloc family ──────────────────
 * Used only when --wrap=malloc is NOT in effect — those calls resolve
 * here and forward to mm_malloc/mm_free directly.  When --wrap IS in
 * effect the linker rewrites references to __real_<sym> to point at
 * the original (renamed) libc symbol, overriding these weak defs. */
__attribute__((weak)) void *__real_malloc(size_t n)
{
	return mm_malloc(USR_HEAP, n);
}
__attribute__((weak)) void *__real_calloc(size_t nmemb, size_t n)
{
	return mm_calloc(USR_HEAP, nmemb, n);
}
__attribute__((weak)) void *__real_realloc(void *p, size_t n)
{
	return mm_realloc(USR_HEAP, p, n);
}
__attribute__((weak)) void *__real_zalloc(size_t n)
{
	return mm_zalloc(USR_HEAP, n);
}
__attribute__((weak)) void *__real_memalign(size_t alignment, size_t size)
{
	return mm_memalign(USR_HEAP, alignment, size);
}
__attribute__((weak)) void __real_free(void *p)
{
	mm_free(USR_HEAP, p);
}

/* ── NuttX kmm_* wrappers ───────────────────────────────────────────── */

extern void *__real_kmm_malloc(size_t n);
extern void *__real_kmm_zalloc(size_t n);
extern void *__real_kmm_calloc(size_t nmemb, size_t n);
extern void *__real_kmm_realloc(void *p, size_t n);
extern void *__real_kmm_memalign(size_t alignment, size_t size);
extern void __real_kmm_free(void *p);

__attribute__((weak)) void *__real_kmm_malloc(size_t n)
{
	return mm_malloc(USR_HEAP, n);
}
__attribute__((weak)) void *__real_kmm_zalloc(size_t n)
{
	return mm_zalloc(USR_HEAP, n);
}
__attribute__((weak)) void *__real_kmm_calloc(size_t nmemb, size_t n)
{
	return mm_calloc(USR_HEAP, nmemb, n);
}
__attribute__((weak)) void *__real_kmm_realloc(void *p, size_t n)
{
	return mm_realloc(USR_HEAP, p, n);
}
__attribute__((weak)) void *__real_kmm_memalign(size_t alignment, size_t size)
{
	return mm_memalign(USR_HEAP, alignment, size);
}
__attribute__((weak)) void __real_kmm_free(void *p)
{
	mm_free(USR_HEAP, p);
}

void *__wrap_kmm_malloc(size_t n)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_alloc_(n);
	return __real_kmm_malloc(n);
}

void *__wrap_kmm_zalloc(size_t n)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_zalloc_(n);
	return __real_kmm_zalloc(n);
}

void *__wrap_kmm_calloc(size_t nmemb, size_t n)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_calloc_(nmemb, n);
	return __real_kmm_calloc(nmemb, n);
}

void *__wrap_kmm_realloc(void *p, size_t n)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_realloc_(p, n);
	return __real_kmm_realloc(p, n);
}

void *__wrap_kmm_memalign(size_t alignment, size_t size)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_memalign_(alignment, size);
	return __real_kmm_memalign(alignment, size);
}

void __wrap_kmm_free(void *p)
{
	if (ove_heap_lock_post_free_(p))
		return;
	__real_kmm_free(p);
}

#endif /* CONFIG_OVE_ZERO_HEAP */

#endif /* CONFIG_OVE_RTOS_NUTTX */
