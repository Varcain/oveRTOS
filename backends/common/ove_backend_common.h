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
#include <errno.h>

#include "ove/types.h"
#include "ove_config.h"

/*
 * Memory allocation wrappers — resolve to backend-specific allocators.
 * FreeRTOS and Zephyr use their own heap; POSIX and NuttX use libc.
 */
#ifdef CONFIG_OVE_ZERO_HEAP
/* In zero-heap mode, any use of the allocator is a compile error.
 * All allocations must use caller-provided or embedded storage. */
static inline void *ove_zero_heap_trap(void) { return (void *)0; }
#define OVE_BACKEND_MALLOC(sz) ove_zero_heap_trap()
#define OVE_BACKEND_FREE(ptr)  ((void)(ptr))
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
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
 * ove_nvs_key_is_valid - validate a caller-supplied NVS key
 * @key: NUL-terminated key string
 *
 * Backends that store NVS entries as files under a directory must
 * reject keys that could escape the NVS root (path traversal) or
 * otherwise form an unsafe filesystem name.
 *
 * Accepts [A-Za-z0-9_.-], rejects empty strings, leading '.' (hidden
 * files / current-directory), and any other byte. Returns true on a
 * safe key, false otherwise.
 */
static inline bool ove_nvs_key_is_valid(const char *key)
{
	if (!key || !*key) return false;
	if (*key == '.') return false;
	for (const char *p = key; *p; ++p) {
		char c = *p;
		bool ok = (c >= 'A' && c <= 'Z') ||
			  (c >= 'a' && c <= 'z') ||
			  (c >= '0' && c <= '9') ||
			  c == '_' || c == '-' || c == '.';
		if (!ok) return false;
	}
	return true;
}

/**
 * ove_errno_to_ove - translate a POSIX errno to an ove error code
 * @e: errno value (typically captured after a failing syscall)
 *
 * Preserves information from the underlying call instead of collapsing
 * every failure into OVE_ERR_NOT_SUPPORTED. Unknown values map to
 * OVE_ERR_NOT_SUPPORTED as a best-effort fallback.
 */
static inline int ove_errno_to_ove(int e)
{
	switch (e) {
	case 0:        return OVE_OK;
	case EINVAL:   return OVE_ERR_INVALID_PARAM;
	case EFAULT:   return OVE_ERR_INVALID_PARAM;
	case ENOENT:   return OVE_ERR_INVALID_PARAM;
	case ENOTDIR:  return OVE_ERR_INVALID_PARAM;
	case EISDIR:   return OVE_ERR_INVALID_PARAM;
	case ERANGE:   return OVE_ERR_INVALID_PARAM;
	case ENAMETOOLONG: return OVE_ERR_INVALID_PARAM;
	case ENOMEM:   return OVE_ERR_NO_MEMORY;
	case ENOSPC:   return OVE_ERR_NO_MEMORY;
	case EDQUOT:   return OVE_ERR_NO_MEMORY;
	case ETIMEDOUT: return OVE_ERR_TIMEOUT;
	case EAGAIN:   return OVE_ERR_TIMEOUT;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK: return OVE_ERR_TIMEOUT;
#endif
	default:       return OVE_ERR_NOT_SUPPORTED;
	}
}

#ifdef CONFIG_OVE_THREAD_STATE_STATS
/**
 * ove_backend_thread_set_state - transition the current thread's state
 * @new_state: one of OVE_THREAD_STATE_* values
 *
 * Backend-internal hook used by shared sync/queue code to mark the
 * caller BLOCKED around a wait (cond_wait, sem_wait) and RUNNING
 * again on wake-up.  Drives the per-state time tracker so
 * `ove_thread_list` reports CPU% correctly.
 *
 * Implemented in the thread backend (posix_thread.c, wasm_thread.c),
 * which owns the thread-local pointer to the current ove_thread.
 * Backends that don't track state compile this away to a no-op.
 */
void ove_backend_thread_set_state(int new_state);
#else
static inline void ove_backend_thread_set_state(int new_state)
{
	(void)new_state;
}
#endif

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
