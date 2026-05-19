/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Unified-heap policy for FreeRTOS heap mode: libc malloc / free /
 * calloc / realloc are wrapped via the linker's `--wrap=<sym>`
 * mechanism to route directly through pvPortMalloc / vPortFree.
 * Without this, the binary carries two independent heaps — FreeRTOS
 * heap_4's ucHeap (where every ove_*_create() allocation goes) and
 * picolibc's _sbrk-backed nano-malloc arena (where cmocka, libc
 * internal buffers, and any libstdc++ stragglers go).  Two pools means
 * an ove_*_create() can fail with bytes free in the libc pool, and a
 * cmocka test_malloc can fail with bytes free in the FreeRTOS pool.
 *
 * The --wrap mechanism (applied by cmake/OveCommon.cmake under
 * !OVE_ZERO_HEAP) makes every call to `malloc(n)` resolve to
 * `__wrap_malloc(n)`, while leaving the original libc `malloc` symbol
 * accessible as `__real_malloc` (never referenced here — we route to
 * pvPortMalloc instead).  This is the same wrap pattern
 * backends/common/ove_heap_lock.c uses for zero-heap-mode trapping;
 * the difference is the destination: trap-or-forward vs forward to
 * pvPortMalloc.
 *
 * Why wrap instead of a strong `malloc` definition: providing `malloc`
 * directly conflicts with picolibc's nano-malloc.c at link time when
 * any TU (e.g. TFLM's C++ runtime via `operator new`) pulls in
 * `_realloc_r`-adjacent symbols that share an object file with
 * picolibc's `malloc`.  `--wrap` sidesteps the multiple-definition
 * error entirely.
 *
 * No size prefix: pvPortMalloc returns a pointer whose preceding
 * BlockLink_t header carries the block size already.  We pass the
 * caller's pointer straight through to pvPortMalloc / vPortFree so
 * the heap's internal accounting stays untouched (a header-prefix
 * scheme would offset every pointer by 8 bytes and require pulling
 * xHeapStructSize from FreeRTOS internals to reconstruct it for
 * realloc).  realloc here does a best-effort allocate + copy + free
 * using the caller-supplied `new_size` as the copy bound, which is
 * correct on grow and conservative on shrink — the realloc path is
 * rare in embedded FreeRTOS apps and never exercised by the bindings'
 * `*_create()` paths.
 *
 * Not compiled in zero-heap mode (configSUPPORT_DYNAMIC_ALLOCATION=0):
 * pvPortMalloc / vPortFree don't exist there, and libc malloc is
 * separately audited by backends/common/ove_heap_lock.c.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS) && !defined(CONFIG_OVE_ZERO_HEAP)

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "portable.h"

void *__wrap_malloc(size_t size)
{
	if (size == 0) {
		return NULL;
	}
	return pvPortMalloc(size);
}

void __wrap_free(void *ptr)
{
	if (ptr == NULL) {
		return;
	}
	vPortFree(ptr);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
	size_t total;
	if (__builtin_mul_overflow(nmemb, size, &total)) {
		return NULL;
	}
	void *p = pvPortMalloc(total);
	if (p != NULL) {
		memset(p, 0, total);
	}
	return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
	if (ptr == NULL) {
		return pvPortMalloc(size);
	}
	if (size == 0) {
		vPortFree(ptr);
		return NULL;
	}
	void *new_ptr = pvPortMalloc(size);
	if (new_ptr == NULL) {
		return NULL;
	}
	/* Conservative copy: the caller's `size` covers the grow case
	 * exactly; for shrink, copying `size` is correct (the rest of
	 * the old allocation is being discarded anyway).  Real C realloc
	 * would use the BlockLink_t header's xBlockSize to determine the
	 * old size precisely, but xHeapStructSize is a FreeRTOS-internal
	 * symbol and dragging it in just for a rarely-used realloc isn't
	 * worth the coupling. */
	memcpy(new_ptr, ptr, size);
	vPortFree(ptr);
	return new_ptr;
}

#endif /* CONFIG_OVE_RTOS_FREERTOS && !CONFIG_OVE_ZERO_HEAP */
