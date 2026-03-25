/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_thread Thread management
 * @brief Create, configure, and query RTOS threads across all supported backends.
 *
 * Two allocation strategies are available:
 *  - @c _create() / @c _destroy() — unified API that works in both heap and
 *    zero-heap mode.  In zero-heap mode these are macros that generate
 *    per-call-site static storage; the stack size must be a compile-time
 *    constant.  @c ove_thread_create() takes three arguments:
 *    @c (handle, stack_sz, desc).
 *  - @c _init() / @c _deinit() — explicit storage control with caller-supplied
 *    buffers.  Use when creating objects in loops, arrays, or structs.
 * @{
 */

#ifndef OVE_THREAD_H
#define OVE_THREAD_H

#include "ove/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Thread entry-point function prototype.
 *
 * @param[in] arg  Caller-supplied context pointer passed from
 *                 ove_thread_desc::arg.
 */
typedef void (*ove_thread_fn)(void *arg);

/**
 * @brief Thread execution state as reported by the active backend.
 */
typedef enum {
	OVE_THREAD_STATE_RUNNING = 0, /**< @brief Currently executing on the CPU. */
	OVE_THREAD_STATE_READY,       /**< @brief Ready to run, waiting for the CPU. */
	OVE_THREAD_STATE_BLOCKED,     /**< @brief Blocked on a synchronisation object or delay. */
	OVE_THREAD_STATE_SUSPENDED,   /**< @brief Explicitly suspended via ove_thread_suspend(). */
	OVE_THREAD_STATE_TERMINATED,  /**< @brief Entry function has returned; not yet destroyed. */
	OVE_THREAD_STATE_UNKNOWN,     /**< @brief State could not be determined. */
} ove_thread_state_t;

/**
 * @brief Per-thread runtime statistics snapshot.
 */
struct ove_thread_stats {
	uint64_t runtime_us;        /**< @brief Total CPU time consumed by this thread in microseconds. */
	uint32_t cpu_percent_x100;  /**< @brief CPU utilisation in hundredths of a percent (e.g. 1250 = 12.50 %). */
};

/**
 * @brief Portable thread-priority levels.
 *
 * Each value maps to a backend-specific numeric priority at initialisation
 * time.  Higher enum values represent higher scheduling priority.
 */
typedef enum {
	OVE_PRIO_IDLE         = 0, /**< @brief Lowest priority; runs only when no other thread is ready. */
	OVE_PRIO_LOW          = 1, /**< @brief Low priority background work. */
	OVE_PRIO_BELOW_NORMAL = 2, /**< @brief Below-normal priority. */
	OVE_PRIO_NORMAL       = 3, /**< @brief Default application priority. */
	OVE_PRIO_ABOVE_NORMAL = 4, /**< @brief Above-normal priority. */
	OVE_PRIO_HIGH         = 5, /**< @brief High priority; prefer for time-sensitive tasks. */
	OVE_PRIO_REALTIME     = 6, /**< @brief Real-time priority; use with care. */
	OVE_PRIO_CRITICAL     = 7, /**< @brief Highest priority; reserved for critical system tasks. */
} ove_prio_t;

/**
 * @brief Thread creation descriptor passed to ove_thread_init() / ove_thread_create().
 */
struct ove_thread_desc {
	const char    *name;       /**< @brief Human-readable thread name (may be truncated by backend). */
	ove_thread_fn  entry;      /**< @brief Thread entry-point function. Must not be NULL. */
	void          *arg;        /**< @brief Opaque argument forwarded to @c entry. May be NULL. */
	ove_prio_t     priority;   /**< @brief Scheduling priority. */
	size_t         stack_size; /**< @brief Stack size in bytes. Must be > 0. */
	void          *stack;      /**< @brief Pointer to caller-allocated stack buffer (static mode only;
	                                       set to NULL for heap mode). */
};

#include "ove/storage.h"

/**
 * @brief Initialise a thread using caller-supplied static storage.
 *
 * Creates a new thread without any heap allocation.  The caller must
 * provide both a @c storage object and a stack buffer via
 * @c desc->stack / @c desc->stack_size.
 *
 * @param[out] handle   Receives the opaque thread handle on success.
 * @param[in]  storage  Pointer to statically allocated backend storage.
 *                      Must remain valid for the lifetime of the thread.
 * @param[in]  desc     Thread descriptor; all fields must be valid.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_thread_deinit, ove_thread_create
 */
int  ove_thread_init(ove_thread_t *handle,
		     ove_thread_storage_t *storage,
		     const struct ove_thread_desc *desc);

/**
 * @brief Terminate and release a thread created with ove_thread_init().
 *
 * Stops the thread and releases any backend-internal resources.  The
 * static storage supplied at init time is not freed.
 *
 * @param[in] handle  Handle returned by ove_thread_init().
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_thread_init
 */
int  ove_thread_deinit(ove_thread_t handle);

/* _create / _destroy — unified across heap and zero-heap modes */
#ifdef OVE_HEAP_THREAD

/**
 * @brief Internal heap-backed thread creation function.
 *
 * Prefer the ove_thread_create() macro which works in both heap and
 * zero-heap mode.  This function is the underlying implementation used
 * in heap mode.
 *
 * @param[out] handle  Receives the opaque thread handle on success.
 * @param[in]  desc    Thread descriptor; @c stack should be NULL.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_thread_create
 */
int  ove_thread_create_(ove_thread_t *handle,
			const struct ove_thread_desc *desc);

/**
 * @brief Stop and free a thread created with ove_thread_create().
 *
 * @param[in] handle  Handle returned by ove_thread_create().
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_thread_create
 */
int  ove_thread_destroy(ove_thread_t handle);

/**
 * @brief Create a thread (works in both heap and zero-heap mode).
 *
 * In heap mode, allocates storage from the heap.  In zero-heap mode,
 * generates per-call-site static storage and an aligned stack buffer.
 *
 * @p stack_sz must be a compile-time integer constant in zero-heap mode.
 * The macro sets @c desc->stack_size; the caller should not set it.
 *
 * @param phandle   Pointer to thread handle to receive the result.
 * @param stack_sz  Stack size in bytes.
 * @param pdesc     Pointer to a thread descriptor.
 */
#define ove_thread_create(phandle, stack_sz, pdesc) \
	({ struct ove_thread_desc _ove_d_ = *(pdesc); \
	   _ove_d_.stack_size = (stack_sz); \
	   ove_thread_create_((phandle), &_ove_d_); })

#elif !defined(__ZIG_CIMPORT__) /* !OVE_HEAP_THREAD — zero-heap mode */

/**
 * @brief Create a thread (works in both heap and zero-heap mode).
 *
 * In zero-heap mode, generates per-call-site static storage and an
 * aligned stack buffer.  @p stack_sz must be a compile-time integer
 * constant.  The macro sets @c desc->stack_size and @c desc->stack;
 * the caller should not set them.
 *
 * @param phandle   Pointer to thread handle to receive the result.
 * @param stack_sz  Stack size in bytes (compile-time constant).
 * @param pdesc     Pointer to a thread descriptor.
 */
#define ove_thread_create(phandle, stack_sz, pdesc) \
	({ static ove_thread_storage_t _ove_stor_; \
	   OVE_THREAD_STACK_BLOCK_STATIC_(_ove_stk_, stack_sz); \
	   struct ove_thread_desc _ove_d_ = *(pdesc); \
	   _ove_d_.stack_size = (stack_sz); \
	   _ove_d_.stack = _ove_stk_; \
	   ove_thread_init((phandle), &_ove_stor_, &_ove_d_); })

#define ove_thread_destroy(handle) ove_thread_deinit(handle)

#endif /* OVE_HEAP_THREAD */

/**
 * @brief Return the handle of the currently executing thread.
 *
 * @return Handle of the calling thread.
 */
ove_thread_t ove_thread_get_self(void);

/**
 * @brief Change the scheduling priority of a thread.
 *
 * @param[in] handle  Thread to modify.
 * @param[in] prio    New priority level.
 */
void ove_thread_set_priority(ove_thread_t handle, ove_prio_t prio);

/**
 * @brief Block the calling thread for at least @p ms milliseconds.
 *
 * @param[in] ms  Minimum sleep duration in milliseconds.  A value of 0
 *                yields the CPU for one scheduler tick.
 */
void ove_thread_sleep_ms(uint32_t ms);

/**
 * @brief Voluntarily yield the CPU to another ready thread of equal or higher priority.
 *
 * Has no effect if no other eligible thread is ready to run.
 */
void ove_thread_yield(void);

/**
 * @brief Start the RTOS scheduler.
 *
 * Must be called after all threads and resources have been created.
 * Typically called indirectly through ove_run().  Does not return on
 * most platforms.
 *
 * @see ove_run
 */
void ove_thread_start_scheduler(void);

/**
 * @brief Suspend a thread, preventing it from being scheduled.
 *
 * The thread remains suspended until ove_thread_resume() is called.
 *
 * @param[in] handle  Thread to suspend.  May be the calling thread itself.
 *
 * @see ove_thread_resume
 */
void ove_thread_suspend(ove_thread_t handle);

/**
 * @brief Resume a previously suspended thread.
 *
 * @param[in] handle  Thread to resume.  Must have been suspended with
 *                    ove_thread_suspend().
 *
 * @see ove_thread_suspend
 */
void ove_thread_resume(ove_thread_t handle);

/**
 * @brief Query how many bytes of stack the thread has used at its high-water mark.
 *
 * @param[in] handle  Thread to inspect.
 * @return Number of bytes consumed at the historical peak, or 0 if the
 *         backend does not support stack profiling.
 */
size_t ove_thread_get_stack_usage(ove_thread_t handle);

/**
 * @brief Query the current execution state of a thread.
 *
 * @param[in] handle  Thread to inspect.
 * @return One of the @c OVE_THREAD_STATE_* values.
 */
ove_thread_state_t ove_thread_get_state(ove_thread_t handle);

/**
 * @brief Retrieve runtime statistics for a thread.
 *
 * Populates @p stats with the total CPU time and utilisation percentage
 * since the scheduler started.
 *
 * @param[in]  handle  Thread to inspect.
 * @param[out] stats   Pointer to a caller-allocated structure that will
 *                     receive the statistics.
 * @return OVE_OK on success, @c OVE_ERR_NOT_SUPPORTED if the backend
 *         does not provide runtime accounting.
 */
int ove_thread_get_runtime_stats(ove_thread_t handle,
				 struct ove_thread_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* OVE_THREAD_H */

/** @} */
