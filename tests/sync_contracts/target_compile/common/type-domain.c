/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include "ove/storage.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__BINDGEN__) || defined(__ZIG_CIMPORT__)
#error "domain controls require complete actual native types"
#endif
#ifndef CONFIG_OVE_SYNC
#error "this control requires SYNC"
#endif
#if !defined(OVE_TYPE_DOMAIN_ONLY) && (defined(CONFIG_OVE_ASYNC) || defined(CONFIG_OVE_TIME) || \
				       defined(CONFIG_OVE_CONSOLE) || defined(CONFIG_OVE_INFER))
#error "this control requires the minimal SYNC profile"
#endif
#if defined(OVE_SYNC_CONTRACT_FREERTOS_H) || defined(OVE_CONDVAR_MODEL_FREERTOS_H) || \
	defined(OVE_SYNC_CONTRACT_ZEPHYR_H) || defined(OVE_SYNC_CONTRACT_NXSEM_H)
#error "SDK models are forbidden in actual target controls"
#endif

#define SIGNATURE(function, type)                                                   \
	_Static_assert(__builtin_types_compatible_p(__typeof__(&(function)), type), \
		       "actual provider signature: " #function)

#ifndef OVE_SYNC_EXPECTED_BITS
#define OVE_SYNC_EXPECTED_BITS 32
#endif
_Static_assert(sizeof(unsigned int) * CHAR_BIT == OVE_SYNC_EXPECTED_BITS,
	       "expected public count width");

_Static_assert(CHAR_BIT == 8 && sizeof(unsigned int) == 4, "current count API is unsigned32");

#if defined(CONFIG_OVE_RTOS_FREERTOS)
#include "FreeRTOS.h"
#include "semphr.h"
_Static_assert(configSUPPORT_STATIC_ALLOCATION == 1, "actual static constructors enabled");
_Static_assert(configUSE_COUNTING_SEMAPHORES == 1, "actual counting constructor enabled");
_Static_assert(configUSE_MUTEXES == 1 && configUSE_RECURSIVE_MUTEXES == 1,
	       "actual mutex constructors enabled");
_Static_assert(sizeof(UBaseType_t) == 4, "actual ARM native count is 32-bit");
_Static_assert((UBaseType_t)-1 > 0, "actual native count is unsigned");
_Static_assert((UBaseType_t)-1 == UINT_MAX, "native and C unsigned ceilings match");
_Static_assert((unsigned int)(UBaseType_t)UINT_MAX == UINT_MAX, "maximum round trip");
SIGNATURE(xQueueCreateCountingSemaphoreStatic,
	  QueueHandle_t (*)(UBaseType_t, UBaseType_t, StaticQueue_t *));
#define PROVIDER_MAX UINT_MAX
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/kernel.h>
_Static_assert(K_SEM_MAX_LIMIT == UINT_MAX, "Zephyr accepts the full unsigned domain");
SIGNATURE(k_mutex_init, int (*)(struct k_mutex *));
SIGNATURE(k_condvar_init, int (*)(struct k_condvar *));
SIGNATURE(k_sem_init, int (*)(struct k_sem *, unsigned int, unsigned int));
#ifdef CONFIG_OBJ_CORE_MUTEX
_Static_assert(offsetof(struct k_mutex, obj_core) < sizeof(struct k_mutex), "mutex core member");
#endif
#ifdef CONFIG_OBJ_CORE_SEM
_Static_assert(offsetof(struct k_sem, obj_core) < sizeof(struct k_sem), "sem core member");
#endif
#ifdef CONFIG_OBJ_CORE_CONDVAR
_Static_assert(offsetof(struct k_condvar, obj_core) < sizeof(struct k_condvar), "CV core member");
#endif
#ifdef CONFIG_OBJ_CORE
SIGNATURE(k_obj_core_unlink, void (*)(struct k_obj_core *));
#endif
#define PROVIDER_MAX K_SEM_MAX_LIMIT
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
_Static_assert(sizeof(int32_t) == 4 && INT32_MAX == INT_MAX, "actual signed32 domain");
_Static_assert(SEM_VALUE_MAX == INT32_MAX, "actual NuttX ceiling");
_Static_assert((uint64_t)SEM_VALUE_MAX + 1u <= UINT_MAX, "exact ceiling+1 is representable");
SIGNATURE(nxsem_init, int (*)(sem_t *, int, int32_t));
SIGNATURE(nxmutex_init, int (*)(mutex_t *));
SIGNATURE(nxrmutex_init, int (*)(rmutex_t *));
#define PROVIDER_MAX SEM_VALUE_MAX
#elif defined(CONFIG_OVE_RTOS_POSIX)
#include <semaphore.h>
_Static_assert(SEM_VALUE_MAX == 2147483647, "the pinned installed POSIX ceiling");
_Static_assert(SEM_VALUE_MAX == INT_MAX, "installed POSIX ceiling is signed32 max");
_Static_assert((uint64_t)SEM_VALUE_MAX + 1u <= UINT_MAX, "exact ceiling+1 is representable");
SIGNATURE(sem_init, int (*)(sem_t *, int, unsigned int));
#define PROVIDER_MAX SEM_VALUE_MAX
#else
#error "select one actual provider"
#endif

#ifndef OVE_SYNC_EXPECTED_MAX
#define OVE_SYNC_EXPECTED_MAX PROVIDER_MAX
#endif
_Static_assert(PROVIDER_MAX == OVE_SYNC_EXPECTED_MAX, "expected provider ceiling");

/* Inspect this retained object data without executing a target program. */
__attribute__((used)) const uint32_t sync_compile_domain_values[] = {
	CHAR_BIT,
	sizeof(unsigned int),
	UINT_MAX,
	PROVIDER_MAX,
	sizeof(ove_mutex_storage_t),
	_Alignof(ove_mutex_storage_t),
	sizeof(ove_sem_storage_t),
	_Alignof(ove_sem_storage_t),
	sizeof(ove_event_storage_t),
	_Alignof(ove_event_storage_t),
	sizeof(ove_condvar_storage_t),
	_Alignof(ove_condvar_storage_t),
};
