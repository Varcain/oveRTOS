/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Stub-backend (POSIX host) extras for the portable heap-lock surface
 * in backends/common/ove_heap_lock.c.  The stub test build does not
 * wire -Wl,--wrap=malloc, so the __real_* references in the wrappers
 * need weak fallbacks that forward to libc directly.  Mirrors the
 * pattern used by freertos_heap_lock.c and zephyr_heap_lock.c.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_ZERO_HEAP

#include <stddef.h>

extern void *malloc(size_t);
extern void free(void *);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);
extern void *memalign(size_t, size_t);

__attribute__((weak)) void *__real_malloc(size_t n)
{
	return malloc(n);
}
__attribute__((weak)) void *__real_calloc(size_t nmemb, size_t n)
{
	return calloc(nmemb, n);
}
__attribute__((weak)) void *__real_realloc(void *p, size_t n)
{
	return realloc(p, n);
}
__attribute__((weak)) void *__real_zalloc(size_t n)
{
	return calloc(1, n);
}
__attribute__((weak)) void *__real_memalign(size_t alignment, size_t size)
{
	return memalign(alignment, size);
}
__attribute__((weak)) void __real_free(void *p)
{
	free(p);
}

#endif /* CONFIG_OVE_ZERO_HEAP */
