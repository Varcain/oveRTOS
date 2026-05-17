/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sync Synchronisation primitives
 * @brief Mutex, counting semaphore, binary event, recursive mutex, and
 *        condition variable APIs.
 *
 * @note All functions in this group require @c CONFIG_OVE_SYNC to be defined.
 *       When @c CONFIG_OVE_SYNC is not set, every function is replaced by a
 *       static inline stub that returns @c OVE_ERR_NOT_SUPPORTED.
 *
 * Two allocation strategies are available for each primitive:
 *  - @c _create() / @c _destroy() — heap-allocated.  Available only when
 *    @c OVE_HEAP_SYNC is defined (i.e. @c CONFIG_OVE_ZERO_HEAP is not set).
 *  - @c _init() / @c _deinit() — caller-supplied storage.  Available in both
 *    modes.  See @c OVE_MUTEX_DEFINE_STATIC and friends for one-step static
 *    helpers.
 * @{
 */

#ifndef OVE_SYNC_H
#define OVE_SYNC_H

#include "ove/types.h"
#include "ove_config.h"
#include "ove/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_SYNC

/* =========================================================================
 * Mutex — _init / _deinit (static storage)
 * ========================================================================= */

/**
 * @brief Initialise a non-recursive mutex using caller-supplied static storage.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[out] mtx      Receives the opaque mutex handle on success.
 * @param[in]  storage  Pointer to statically allocated backend storage.
 *                      Must remain valid for the lifetime of the mutex.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_mutex_deinit, ove_mutex_create, ove_mutex_lock, ove_mutex_unlock
 */
OVE_NODISCARD int ove_mutex_init(ove_mutex_t *mtx, ove_mutex_storage_t *storage) OVE_NONNULL(1, 2);

/**
 * @brief Release resources held by a mutex initialised with ove_mutex_init().
 *
 * Every backend MUST release any kernel-side resources associated with
 * the mutex (e.g. destroy semaphores, free kernel handles). The static
 * storage supplied at init time is not freed — the caller owns it.
 * After @c ove_mutex_deinit() returns, the handle is invalid; calling any
 * other mutex operation on it is undefined.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] mtx  Handle returned by ove_mutex_init().
 *
 * @see ove_mutex_init
 */
void ove_mutex_deinit(ove_mutex_t mtx);

/* =========================================================================
 * Semaphore — _init / _deinit (static storage)
 * ========================================================================= */

/**
 * @brief Initialise a counting semaphore using caller-supplied static storage.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[out] sem      Receives the opaque semaphore handle on success.
 * @param[in]  storage  Pointer to statically allocated backend storage.
 *                      Must remain valid for the lifetime of the semaphore.
 * @param[in]  initial  Initial count value.
 * @param[in]  max      Maximum count value.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_sem_deinit, ove_sem_create, ove_sem_take, ove_sem_give
 */
OVE_NODISCARD int ove_sem_init(ove_sem_t *sem, ove_sem_storage_t *storage, unsigned int initial,
			       unsigned int max) OVE_NONNULL(1, 2);

/**
 * @brief Release resources held by a semaphore initialised with ove_sem_init().
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] sem  Handle returned by ove_sem_init().
 *
 * @see ove_sem_init
 */
void ove_sem_deinit(ove_sem_t sem);

/* =========================================================================
 * Binary event — _init / _deinit (static storage)
 * ========================================================================= */

/**
 * @brief Initialise a binary event object using caller-supplied static storage.
 *
 * A binary event starts in the unsignalled state.  One waiter is unblocked
 * per ove_event_signal() call.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[out] evt      Receives the opaque event handle on success.
 * @param[in]  storage  Pointer to statically allocated backend storage.
 *                      Must remain valid for the lifetime of the event.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_event_deinit, ove_event_create, ove_event_wait, ove_event_signal
 */
OVE_NODISCARD int ove_event_init(ove_event_t *evt, ove_event_storage_t *storage) OVE_NONNULL(1, 2);

/**
 * @brief Release resources held by an event initialised with ove_event_init().
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] evt  Handle returned by ove_event_init().
 *
 * @see ove_event_init
 */
void ove_event_deinit(ove_event_t evt);

/* =========================================================================
 * Recursive mutex — _init (static storage)
 * ========================================================================= */

/**
 * @brief Initialise a recursive mutex using caller-supplied static storage.
 *
 * A recursive mutex may be locked multiple times by the same thread without
 * deadlocking.  Each successful lock must be paired with an unlock.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[out] mtx      Receives the opaque mutex handle on success.
 * @param[in]  storage  Pointer to statically allocated backend storage.
 *                      Must remain valid for the lifetime of the mutex.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_mutex_deinit, ove_recursive_mutex_create,
 *      ove_recursive_mutex_lock, ove_recursive_mutex_unlock
 */
OVE_NODISCARD int ove_recursive_mutex_init(ove_mutex_t *mtx,
					   ove_mutex_storage_t *storage) OVE_NONNULL(1, 2);

/* =========================================================================
 * Condition variable — _init / _deinit (static storage)
 * ========================================================================= */

/**
 * @brief Initialise a condition variable using caller-supplied static storage.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[out] cv       Receives the opaque condition variable handle on success.
 * @param[in]  storage  Pointer to statically allocated backend storage.
 *                      Must remain valid for the lifetime of the condvar.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_condvar_deinit, ove_condvar_create, ove_condvar_wait,
 *      ove_condvar_signal, ove_condvar_broadcast
 */
OVE_NODISCARD int ove_condvar_init(ove_condvar_t *cv,
				   ove_condvar_storage_t *storage) OVE_NONNULL(1, 2);

/**
 * @brief Release resources held by a condition variable initialised with
 *        ove_condvar_init().
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] cv  Handle returned by ove_condvar_init().
 *
 * @see ove_condvar_init
 */
void ove_condvar_deinit(ove_condvar_t cv);

/* =========================================================================
 * Heap-gated _create / _destroy variants
 * ========================================================================= */

#ifdef OVE_HEAP_SYNC

/**
 * @brief Allocate and initialise a non-recursive mutex from the heap.
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC
 *       (i.e. @c CONFIG_OVE_ZERO_HEAP must not be set).
 *
 * @param[out] mtx  Receives the opaque mutex handle on success.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_mutex_destroy, ove_mutex_init
 */
OVE_NODISCARD int ove_mutex_create(ove_mutex_t *mtx) OVE_NONNULL(1);

/**
 * @brief Destroy and free a mutex allocated with ove_mutex_create().
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[in] mtx  Handle returned by ove_mutex_create().
 *
 * @see ove_mutex_create
 */
void ove_mutex_destroy(ove_mutex_t mtx);

/**
 * @brief Allocate and initialise a counting semaphore from the heap.
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[out] sem      Receives the opaque semaphore handle on success.
 * @param[in]  initial  Initial count value.
 * @param[in]  max      Maximum count value.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_sem_destroy, ove_sem_init
 */
OVE_NODISCARD int ove_sem_create(ove_sem_t *sem, unsigned int initial,
				 unsigned int max) OVE_NONNULL(1);

/**
 * @brief Destroy and free a semaphore allocated with ove_sem_create().
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[in] sem  Handle returned by ove_sem_create().
 *
 * @see ove_sem_create
 */
void ove_sem_destroy(ove_sem_t sem);

/**
 * @brief Allocate and initialise a binary event from the heap.
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[out] evt  Receives the opaque event handle on success.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_event_destroy, ove_event_init
 */
OVE_NODISCARD int ove_event_create(ove_event_t *evt) OVE_NONNULL(1);

/**
 * @brief Destroy and free an event allocated with ove_event_create().
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[in] evt  Handle returned by ove_event_create().
 *
 * @see ove_event_create
 */
void ove_event_destroy(ove_event_t evt);

/**
 * @brief Allocate and initialise a recursive mutex from the heap.
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[out] mtx  Receives the opaque mutex handle on success.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_recursive_mutex_destroy, ove_recursive_mutex_init
 */
OVE_NODISCARD int ove_recursive_mutex_create(ove_mutex_t *mtx) OVE_NONNULL(1);

/**
 * @brief Destroy and free a recursive mutex allocated with
 *        ove_recursive_mutex_create().
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[in] mtx  Handle returned by ove_recursive_mutex_create().
 *
 * @see ove_recursive_mutex_create
 */
void ove_recursive_mutex_destroy(ove_mutex_t mtx);

/**
 * @brief Allocate and initialise a condition variable from the heap.
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[out] cv  Receives the opaque condition variable handle on success.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_condvar_destroy, ove_condvar_init
 */
OVE_NODISCARD int ove_condvar_create(ove_condvar_t *cv) OVE_NONNULL(1);

/**
 * @brief Destroy and free a condition variable allocated with
 *        ove_condvar_create().
 *
 * @note Requires @c CONFIG_OVE_SYNC and @c OVE_HEAP_SYNC.
 *
 * @param[in] cv  Handle returned by ove_condvar_create().
 *
 * @see ove_condvar_create
 */
void ove_condvar_destroy(ove_condvar_t cv);

#endif /* OVE_HEAP_SYNC */

/* =========================================================================
 * Operations — always available (when CONFIG_OVE_SYNC is set)
 * ========================================================================= */

/**
 * @brief Acquire a non-recursive mutex, blocking until it is available or
 *        the timeout expires.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] mtx         Mutex handle obtained from ove_mutex_init() or
 *                        ove_mutex_create().
 * @param[in] timeout_ms  Maximum time to wait in milliseconds.  Pass
 *                        @c OVE_WAIT_FOREVER to block indefinitely.
 * @return OVE_OK on success, @c OVE_ERR_TIMEOUT if the deadline was
 *         reached, or another negative error code on failure.
 *
 * @see ove_mutex_unlock
 */
OVE_NODISCARD int ove_mutex_lock(ove_mutex_t mtx, uint32_t timeout_ms) OVE_NONNULL(1);

/**
 * @brief Release a non-recursive mutex previously acquired by ove_mutex_lock().
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] mtx  Mutex handle to release.
 *
 * @see ove_mutex_lock
 */
void ove_mutex_unlock(ove_mutex_t mtx);

/**
 * @brief Decrement (take) a semaphore, blocking until a count is available
 *        or the timeout expires.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] sem         Semaphore handle obtained from ove_sem_init() or
 *                        ove_sem_create().
 * @param[in] timeout_ms  Maximum time to wait in milliseconds.  Pass
 *                        @c OVE_WAIT_FOREVER to block indefinitely.
 * @return OVE_OK on success, @c OVE_ERR_TIMEOUT if the deadline was
 *         reached, or another negative error code on failure.
 *
 * @see ove_sem_give
 */
OVE_NODISCARD int ove_sem_take(ove_sem_t sem, uint32_t timeout_ms) OVE_NONNULL(1);

/**
 * @brief Increment (give) a semaphore, potentially unblocking a waiting thread.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] sem  Semaphore handle to increment.
 *
 * @see ove_sem_take
 */
void ove_sem_give(ove_sem_t sem);

/**
 * @brief Wait for a binary event to be signalled.
 *
 * Blocks the calling thread until ove_event_signal() or
 * ove_event_signal_from_isr() is called on @p evt, or until the timeout
 * expires.  The event is automatically reset (consumed) after a successful
 * wait.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] evt         Event handle obtained from ove_event_init() or
 *                        ove_event_create().
 * @param[in] timeout_ms  Maximum time to wait in milliseconds.  Pass
 *                        @c OVE_WAIT_FOREVER to block indefinitely.
 * @return OVE_OK on success, @c OVE_ERR_TIMEOUT if the deadline was
 *         reached, or another negative error code on failure.
 *
 * @see ove_event_signal, ove_event_signal_from_isr
 */
OVE_NODISCARD int ove_event_wait(ove_event_t evt, uint32_t timeout_ms) OVE_NONNULL(1);

/**
 * @brief Signal a binary event, unblocking one waiting thread.
 *
 * Safe to call from any thread context.  Must @b not be called from an
 * ISR — use ove_event_signal_from_isr() instead.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] evt  Event handle to signal.
 *
 * @see ove_event_wait, ove_event_signal_from_isr
 */
void ove_event_signal(ove_event_t evt);

/**
 * @brief Signal a binary event from an interrupt service routine.
 *
 * ISR-safe variant of ove_event_signal().  May trigger a context switch
 * to a higher-priority thread after the ISR exits.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] evt  Event handle to signal.
 *
 * @see ove_event_signal, ove_event_wait
 */
void ove_event_signal_from_isr(ove_event_t evt);

/**
 * @brief Acquire a recursive mutex, blocking until it is available or the
 *        timeout expires.
 *
 * The same thread may call this function multiple times without deadlocking.
 * Each successful lock must be balanced by a call to
 * ove_recursive_mutex_unlock().
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] mtx         Recursive mutex handle obtained from
 *                        ove_recursive_mutex_init() or
 *                        ove_recursive_mutex_create().
 * @param[in] timeout_ms  Maximum time to wait in milliseconds.  Pass
 *                        @c OVE_WAIT_FOREVER to block indefinitely.
 * @return OVE_OK on success, @c OVE_ERR_TIMEOUT if the deadline was
 *         reached, or another negative error code on failure.
 *
 * @see ove_recursive_mutex_unlock
 */
OVE_NODISCARD int ove_recursive_mutex_lock(ove_mutex_t mtx, uint32_t timeout_ms) OVE_NONNULL(1);

/**
 * @brief Release one level of a recursive mutex lock.
 *
 * Decrements the recursive lock count.  The mutex is fully released and
 * made available to other threads only when the count reaches zero.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] mtx  Recursive mutex handle to unlock.
 *
 * @see ove_recursive_mutex_lock
 */
void ove_recursive_mutex_unlock(ove_mutex_t mtx);

/**
 * @brief Atomically release a mutex and wait on a condition variable.
 *
 * The mutex @p mtx must be held by the calling thread before this call.
 * It is released atomically as the thread begins waiting.  When the
 * function returns (either due to a signal or timeout), @p mtx is
 * re-acquired before returning to the caller.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] cv          Condition variable handle obtained from
 *                        ove_condvar_init() or ove_condvar_create().
 * @param[in] mtx         Mutex that guards the condition.  Must be locked
 *                        by the calling thread.
 * @param[in] timeout_ms  Maximum time to wait in milliseconds.  Pass
 *                        @c OVE_WAIT_FOREVER to block indefinitely.
 * @return OVE_OK on success, @c OVE_ERR_TIMEOUT if the deadline was
 *         reached, or another negative error code on failure.
 *
 * @see ove_condvar_signal, ove_condvar_broadcast
 */
OVE_NODISCARD int ove_condvar_wait(ove_condvar_t cv, ove_mutex_t mtx,
				   uint32_t timeout_ms) OVE_NONNULL(1, 2);

/**
 * @brief Wake one thread waiting on a condition variable.
 *
 * If no threads are waiting, the signal is lost (not stored).
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] cv  Condition variable handle to signal.
 *
 * @see ove_condvar_wait, ove_condvar_broadcast
 */
void ove_condvar_signal(ove_condvar_t cv);

/**
 * @brief Wake all threads waiting on a condition variable.
 *
 * @note Requires @c CONFIG_OVE_SYNC.
 *
 * @param[in] cv  Condition variable handle to broadcast on.
 *
 * @see ove_condvar_wait, ove_condvar_signal
 */
void ove_condvar_broadcast(ove_condvar_t cv);

#else /* !CONFIG_OVE_SYNC */

/* P0-3: _init/_deinit stubs so OVE_*_DEFINE_STATIC links cleanly when
 * CONFIG_OVE_SYNC=n. The macros expand to a constructor that calls the
 * _init function unconditionally; without these stubs, the link fails. */
static inline int ove_mutex_init(ove_mutex_t *m, ove_mutex_storage_t *s)
{
	(void)m;
	(void)s;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_mutex_deinit(ove_mutex_t m)
{
	(void)m;
}
static inline int ove_sem_init(ove_sem_t *s, ove_sem_storage_t *st, unsigned int i,
			       unsigned int x)
{
	(void)s;
	(void)st;
	(void)i;
	(void)x;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_sem_deinit(ove_sem_t s)
{
	(void)s;
}
static inline int ove_event_init(ove_event_t *e, ove_event_storage_t *s)
{
	(void)e;
	(void)s;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_event_deinit(ove_event_t e)
{
	(void)e;
}
static inline int ove_recursive_mutex_init(ove_mutex_t *m, ove_mutex_storage_t *s)
{
	(void)m;
	(void)s;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_condvar_init(ove_condvar_t *c, ove_condvar_storage_t *s)
{
	(void)c;
	(void)s;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_condvar_deinit(ove_condvar_t c)
{
	(void)c;
}

static inline int ove_mutex_create(ove_mutex_t *m)
{
	(void)m;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_mutex_destroy(ove_mutex_t m)
{
	(void)m;
}
static inline int ove_mutex_lock(ove_mutex_t m, uint32_t t)
{
	(void)m;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_mutex_unlock(ove_mutex_t m)
{
	(void)m;
}
static inline int ove_sem_create(ove_sem_t *s, unsigned int i, unsigned int x)
{
	(void)s;
	(void)i;
	(void)x;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_sem_destroy(ove_sem_t s)
{
	(void)s;
}
static inline int ove_sem_take(ove_sem_t s, uint32_t t)
{
	(void)s;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_sem_give(ove_sem_t s)
{
	(void)s;
}
static inline int ove_event_create(ove_event_t *e)
{
	(void)e;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_event_destroy(ove_event_t e)
{
	(void)e;
}
static inline int ove_event_wait(ove_event_t e, uint32_t t)
{
	(void)e;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_event_signal(ove_event_t e)
{
	(void)e;
}
static inline void ove_event_signal_from_isr(ove_event_t e)
{
	(void)e;
}
static inline int ove_recursive_mutex_create(ove_mutex_t *m)
{
	(void)m;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_recursive_mutex_lock(ove_mutex_t m, uint32_t t)
{
	(void)m;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_recursive_mutex_unlock(ove_mutex_t m)
{
	(void)m;
}
static inline void ove_recursive_mutex_destroy(ove_mutex_t m)
{
	(void)m;
}
static inline int ove_condvar_create(ove_condvar_t *c)
{
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_condvar_destroy(ove_condvar_t c)
{
	(void)c;
}
static inline int ove_condvar_wait(ove_condvar_t c, ove_mutex_t m, uint32_t t)
{
	(void)c;
	(void)m;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_condvar_signal(ove_condvar_t c)
{
	(void)c;
}
static inline void ove_condvar_broadcast(ove_condvar_t c)
{
	(void)c;
}

#endif /* CONFIG_OVE_SYNC */

#ifdef __cplusplus
}
#endif

#endif /* OVE_SYNC_H */

/** @} */
