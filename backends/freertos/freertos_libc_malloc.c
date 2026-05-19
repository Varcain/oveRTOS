/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Unified-heap policy for FreeRTOS heap mode: libc malloc / free /
 * calloc / realloc are wrapped via the linker's `--wrap=<sym>`
 * mechanism to route through pvPortMalloc / vPortFree.  Without this,
 * the binary carries two independent heaps — FreeRTOS heap_4's ucHeap
 * (where every ove_*_create() allocation goes) and picolibc's
 * _sbrk-backed nano-malloc arena (where cmocka, libc internal buffers,
 * and any libstdc++ stragglers go).  Two pools means an ove_*_create()
 * can fail with bytes free in the libc pool, and a cmocka test_malloc
 * can fail with bytes free in the FreeRTOS pool.
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
 * Size prefix: pvPortMalloc has no per-block size query, so realloc
 * would have no old-size to copy from.  We allocate
 * `user_size + sizeof(size_t)`, stash the user size in the first
 * sizeof(size_t) bytes, and return the pointer offset past the header.
 * The 8-byte alignment from pvPortMalloc plus an 8-byte header
 * preserves max_align_t alignment on the user pointer (Cortex-M with
 * FPU expects 8-byte alignment for doubles / NEON-equivalent).
 *
 * Not compiled in zero-heap mode (configSUPPORT_DYNAMIC_ALLOCATION=0):
 * pvPortMalloc / vPortFree don't exist there, and libc malloc is
 * separately audited by backends/common/ove_heap_lock.c.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS) && !defined(CONFIG_OVE_ZERO_HEAP)

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "portable.h"

/* 8-byte header preserves Cortex-M max_align_t alignment on the user
 * pointer.  Only the first sizeof(size_t) bytes carry the user size;
 * the rest is alignment padding. */
#define OVE_LIBC_MALLOC_HEADER 8

void *__wrap_malloc(size_t size)
{
	if (size == 0) {
		return NULL;
	}
	size_t total = size + OVE_LIBC_MALLOC_HEADER;
	if (total < size) { /* overflow */
		return NULL;
	}
	uint8_t *raw = pvPortMalloc(total);
	if (raw == NULL) {
		return NULL;
	}
	*(size_t *)raw = size;
	return raw + OVE_LIBC_MALLOC_HEADER;
}

void __wrap_free(void *ptr)
{
	if (ptr == NULL) {
		return;
	}
	vPortFree((uint8_t *)ptr - OVE_LIBC_MALLOC_HEADER);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
	size_t total;
	if (__builtin_mul_overflow(nmemb, size, &total)) {
		return NULL;
	}
	void *p = __wrap_malloc(total);
	if (p != NULL) {
		memset(p, 0, total);
	}
	return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
	if (ptr == NULL) {
		return __wrap_malloc(size);
	}
	if (size == 0) {
		__wrap_free(ptr);
		return NULL;
	}
	size_t old_size = *((size_t *)ptr - 1);
	void *new_ptr = __wrap_malloc(size);
	if (new_ptr == NULL) {
		return NULL;
	}
	memcpy(new_ptr, ptr, old_size < size ? old_size : size);
	__wrap_free(ptr);
	return new_ptr;
}

#endif /* CONFIG_OVE_RTOS_FREERTOS && !CONFIG_OVE_ZERO_HEAP */
