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
 */
extern void *malloc(size_t);
extern void  free(void *);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);
extern void *memalign(size_t, size_t);

__attribute__((weak)) void *__real_malloc(size_t n)
	{ return malloc(n); }
__attribute__((weak)) void *__real_calloc(size_t nmemb, size_t n)
	{ return calloc(nmemb, n); }
__attribute__((weak)) void *__real_realloc(void *p, size_t n)
	{ return realloc(p, n); }
__attribute__((weak)) void *__real_zalloc(size_t n)
	{ return calloc(1, n); }
__attribute__((weak)) void *__real_memalign(size_t alignment, size_t size)
	{ return memalign(alignment, size); }
__attribute__((weak)) void  __real_free(void *p)
	{ free(p); }

#endif /* CONFIG_OVE_RTOS_ZEPHYR && CONFIG_OVE_ZERO_HEAP */
