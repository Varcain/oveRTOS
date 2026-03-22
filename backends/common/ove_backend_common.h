/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_BACKEND_COMMON_H
#define OVE_BACKEND_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#include "ove/types.h"
#include "ove_config.h"

/*
 * Memory allocation wrappers — resolve to backend-specific allocators.
 * FreeRTOS and Zephyr use their own heap; POSIX and NuttX use libc.
 */
#if defined(CONFIG_OVE_RTOS_FREERTOS)
#include "FreeRTOS.h"
#define OVE_BACKEND_MALLOC(sz) pvPortMalloc(sz)
#define OVE_BACKEND_FREE(ptr)  vPortFree(ptr)
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/kernel.h>
#define OVE_BACKEND_MALLOC(sz) k_malloc(sz)
#define OVE_BACKEND_FREE(ptr)  k_free(ptr)
#else
/* POSIX and NuttX both use standard libc allocation */
#include <stdlib.h>
#define OVE_BACKEND_MALLOC(sz) malloc(sz)
#define OVE_BACKEND_FREE(ptr)  free(ptr)
#endif

/**
 * ove_timeout_is_forever - check if a timeout value means "wait forever"
 * @ms: timeout in milliseconds
 *
 * Returns true when the caller requested an infinite wait.
 */
static inline bool ove_timeout_is_forever(uint32_t ms)
{
	return ms == OVE_WAIT_FOREVER;
}

/**
 * ove_check_param - validate that a pointer is non-NULL
 * @ptr: pointer to check
 *
 * Returns OVE_OK on success, OVE_ERR_INVALID_PARAM if NULL.
 */
static inline int ove_check_param(const void *ptr)
{
	return ptr ? OVE_OK : OVE_ERR_INVALID_PARAM;
}

/**
 * ove_alloc_or_use - use caller-provided storage or allocate from heap
 * @storage: caller-provided buffer, or NULL to allocate
 * @size:    number of bytes needed
 *
 * If @storage is non-NULL it is returned as-is (static/stack allocation).
 * Otherwise a heap block of @size bytes is allocated via the backend allocator.
 * Returns NULL on allocation failure.
 */
static inline void *ove_alloc_or_use(void *storage, size_t size)
{
	return storage ? storage : OVE_BACKEND_MALLOC(size);
}

/**
 * OVE_CHECK_PARAMS_2 - validate two pointers are non-NULL
 */
#define OVE_CHECK_PARAMS_2(a, b) \
	do { if (!(a) || !(b)) return OVE_ERR_INVALID_PARAM; } while(0)

/**
 * OVE_CHECK_PARAMS_3 - validate three pointers are non-NULL
 */
#define OVE_CHECK_PARAMS_3(a, b, c) \
	do { if (!(a) || !(b) || !(c)) return OVE_ERR_INVALID_PARAM; } while(0)

/**
 * OVE_CREATE_IMPL - common create pattern: malloc, check, assign, init
 * @type:       struct type name (without 'struct' prefix)
 * @handle_out: pointer to handle variable to assign
 * @init_call:  init function call expression using 'w' as the allocated ptr
 */
#define OVE_CREATE_IMPL(type, handle_out, init_call) \
	do { \
		struct type *w = OVE_BACKEND_MALLOC(sizeof(*w)); \
		if (!w) return OVE_ERR_NO_MEMORY; \
		*(handle_out) = w; \
		return (init_call); \
	} while(0)

#endif /* OVE_BACKEND_COMMON_H */
