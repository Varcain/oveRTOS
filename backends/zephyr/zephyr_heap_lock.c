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
 * Zephyr zero-heap applications disable the kernel heap and keep the
 * libc malloc arena enabled. Application links leave libc allocators
 * unwrapped, so ove_heap_lock() has no effect. The QEMU and Renode
 * zero-heap test fixtures link the wrappers, so wrapped allocations
 * return NULL after ove_heap_lock(). Calls to free() remain allowed.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_RTOS_ZEPHYR) && defined(CONFIG_OVE_ZERO_HEAP)

#include <stddef.h>

/*
 * Weak __real_* fallbacks — same pattern as the FreeRTOS backend.
 * Forward to the libc allocators when --wrap isn't wired. The linker's
 * strong __real_* aliases override these when --wrap is in effect.
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
extern void free(void *);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);

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
__attribute__((weak)) void __real_free(void *p)
{
	free(p);
}

#endif /* CONFIG_OVE_RTOS_ZEPHYR && CONFIG_OVE_ZERO_HEAP */
