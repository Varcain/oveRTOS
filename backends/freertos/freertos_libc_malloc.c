/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Unified-heap policy for FreeRTOS heap mode: libc malloc / free /
 * calloc / realloc are wrapped to route through pvPortMalloc /
 * vPortFree.  Without this, the binary carries two independent heaps —
 * FreeRTOS heap_4's ucHeap (where every ove_*_create() allocation goes)
 * and picolibc's _sbrk-backed nano-malloc arena (where cmocka, libc
 * internal buffers, and any libstdc++ stragglers go).  Two pools means
 * an ove_*_create() can fail with bytes free in the libc pool, and a
 * cmocka test_malloc can fail with bytes free in the FreeRTOS pool.
 *
 * Wrapping libc malloc onto pvPortMalloc unifies them: the kernel owns
 * the single heap, libc is just another consumer.  Side benefit: the
 * picolibc _sbrk arena and its hardcoded RAM region disappear, freeing
 * SRAM for BSS.
 *
 * Size prefix: pvPortMalloc has no per-block size query, so realloc
 * would have no old-size to copy from.  We allocate
 * `user_size + sizeof(size_t)`, stash the user size in the first
 * sizeof(size_t) bytes, and return the pointer offset past the header.
 * The 8-byte alignment from pvPortMalloc plus an 8-byte header
 * preserves max_align_t alignment on the user pointer (Cortex-M
 * with FPU expects 8-byte alignment for doubles / NEON-equivalent).
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

void *malloc(size_t size)
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

void free(void *ptr)
{
	if (ptr == NULL) {
		return;
	}
	vPortFree((uint8_t *)ptr - OVE_LIBC_MALLOC_HEADER);
}

void *calloc(size_t nmemb, size_t size)
{
	size_t total;
	if (__builtin_mul_overflow(nmemb, size, &total)) {
		return NULL;
	}
	void *p = malloc(total);
	if (p != NULL) {
		memset(p, 0, total);
	}
	return p;
}

void *realloc(void *ptr, size_t size)
{
	if (ptr == NULL) {
		return malloc(size);
	}
	if (size == 0) {
		free(ptr);
		return NULL;
	}
	size_t old_size = *((size_t *)ptr - 1);
	void *new_ptr = malloc(size);
	if (new_ptr == NULL) {
		return NULL;
	}
	memcpy(new_ptr, ptr, old_size < size ? old_size : size);
	free(ptr);
	return new_ptr;
}

#endif /* CONFIG_OVE_RTOS_FREERTOS && !CONFIG_OVE_ZERO_HEAP */
