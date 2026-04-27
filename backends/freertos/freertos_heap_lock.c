/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * FreeRTOS backend extras for the portable heap-lock surface in
 * backends/common/ove_heap_lock.c.
 *
 * In zero-heap mode FreeRTOS itself runs with
 * configSUPPORT_DYNAMIC_ALLOCATION=0 — pvPortMalloc / vPortFree are
 * not provided by the kernel.  newlib (or picolibc) still provides
 * the libc malloc family; backends/common/ove_heap_lock.c wraps
 * those.  When --wrap is wired, the linker rewrites __real_<sym>
 * references to the renamed libc symbol.  When --wrap is not wired
 * (e.g. some test build paths), the weak fallbacks below let the
 * wrap functions stay callable for direct test invocation — they
 * abort with a clear error rather than silently re-entering libc
 * (which would create the same recursion the NuttX path avoids).
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS) && defined(CONFIG_OVE_ZERO_HEAP)

#include <stddef.h>

/*
 * Weak __real_* fallbacks for the case when --wrap=malloc is NOT
 * wired into the final link (no linker-generated strong alias to
 * the original libc malloc).  When --wrap IS wired, the linker
 * rewrites these references to point at the renamed-original libc
 * symbol; the strong alias overrides our weak.  When --wrap is OFF,
 * we forward to libc directly — no recursion risk because libc
 * malloc / calloc / etc. are not wrapped.
 *
 * In FreeRTOS zero-heap configSUPPORT_DYNAMIC_ALLOCATION=0 disables
 * pvPortMalloc; the libc heap (newlib _sbrk over _Min_Heap_Size, or
 * picolibc's static arena) is what malloc actually backs.
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
	{ /* No POSIX zalloc — emulate via calloc(1, n). */
	  return calloc(1, n); }
__attribute__((weak)) void *__real_memalign(size_t alignment, size_t size)
	{ return memalign(alignment, size); }
__attribute__((weak)) void  __real_free(void *p)
	{ free(p); }

#endif /* CONFIG_OVE_RTOS_FREERTOS && CONFIG_OVE_ZERO_HEAP */
