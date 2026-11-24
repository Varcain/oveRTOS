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
 * FreeRTOS heap-mode builds always link heap_4. The realloc wrapper
 * depends on its block header to determine the old usable size.
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
#include "task.h"
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

/* heap_4's block size includes the aligned BlockLink_t header.
 * Its top bit marks the block as allocated. */
static size_t heap4_usable_size(const void *ptr)
{
	struct heap4_link {
		void *next;
		size_t size;
	};
	const size_t header_size = (sizeof(struct heap4_link) + portBYTE_ALIGNMENT - 1) &
				   ~(size_t)portBYTE_ALIGNMENT_MASK;
	const struct heap4_link *link = (const void *)((const uint8_t *)ptr - header_size);
	const size_t allocated_bit = (size_t)1 << (sizeof(size_t) * 8 - 1);

	configASSERT((link->size & allocated_bit) != 0);
	return (link->size & ~allocated_bit) - header_size;
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
	const size_t old_usable = heap4_usable_size(ptr);
	memcpy(new_ptr, ptr, old_usable < size ? old_usable : size);
	vPortFree(ptr);
	return new_ptr;
}

#endif /* CONFIG_OVE_RTOS_FREERTOS && !CONFIG_OVE_ZERO_HEAP */
