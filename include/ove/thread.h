/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file thread.h
 * @defgroup ove_thread Thread management
 * @brief Create, configure, and query RTOS threads across all supported backends.
 *
 * Two allocation strategies are available:
 *  - @c _create() / @c _destroy() — heap-allocated (storage + stack).
 *    Available only when @c OVE_HEAP_THREAD is defined (i.e.
 *    @c CONFIG_OVE_ZERO_HEAP is not set).
 *  - @c _init() / @c _deinit() — caller-supplied storage and stack buffer.
 *    Available in both modes.  See @c OVE_THREAD_DEFINE_STATIC for a
 *    one-step static helper.
 * @{
 */

#ifndef OVE_THREAD_H
#define OVE_THREAD_H

#include "ove/types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Thread entry-point function prototype.
 *
 * @param[in] arg  Caller-supplied context pointer passed at creation time.
 */
typedef void (*ove_thread_fn)(void *arg);

/**
 * @brief Thread execution state as reported by the active backend.
 */
typedef enum {
	OVE_THREAD_STATE_RUNNING = 0, /**< @brief Currently executing on the CPU. */
	OVE_THREAD_STATE_READY,	      /**< @brief Ready to run, waiting for the CPU. */
	OVE_THREAD_STATE_BLOCKED,     /**< @brief Blocked on a synchronisation object or delay. */
	OVE_THREAD_STATE_SUSPENDED,   /**< @brief Explicitly suspended via ove_thread_suspend(). */
	OVE_THREAD_STATE_TERMINATED,  /**< @brief Entry function has returned; not yet destroyed. */
	OVE_THREAD_STATE_UNKNOWN,     /**< @brief State could not be determined. */
} ove_thread_state_t;

/**
 * @brief Per-thread runtime statistics snapshot.
 */
struct ove_thread_stats {
	uint64_t runtime_us; /**< @brief Total CPU time consumed by this thread in microseconds. */
	uint32_t cpu_percent_x100; /**< @brief CPU utilisation in hundredths of a percent (e.g. 1250 = 12.50 %). */
};

/**
 * @brief Portable thread-priority levels.
 *
 * Each value maps to a backend-specific numeric priority at initialisation
 * time.  Higher enum values represent higher scheduling priority.
 */
typedef enum {
	OVE_PRIO_IDLE = 0, /**< @brief Lowest priority; runs only when no other thread is ready. */
	OVE_PRIO_LOW = 1,  /**< @brief Low priority background work. */
	OVE_PRIO_BELOW_NORMAL = 2, /**< @brief Below-normal priority. */
	OVE_PRIO_NORMAL = 3,	   /**< @brief Default application priority. */
	OVE_PRIO_ABOVE_NORMAL = 4, /**< @brief Above-normal priority. */
	OVE_PRIO_HIGH = 5,	   /**< @brief High priority; prefer for time-sensitive tasks. */
	OVE_PRIO_REALTIME = 6,	   /**< @brief Real-time priority; use with care. */
	OVE_PRIO_CRITICAL = 7, /**< @brief Highest priority; reserved for critical system tasks. */
} ove_prio_t;

#include "ove/storage.h"

/**
 * @brief Initialise a thread using caller-supplied static storage and stack.
 *
 * Creates a new thread without any heap allocation.  The caller must
 * provide a backend @p storage object and a @p stack buffer of @p stack_size
 * bytes.
 *
 * @param[out] handle      Receives the opaque thread handle on success.
 * @param[in]  storage     Pointer to statically allocated backend storage.
 *                         Must remain valid for the lifetime of the thread.
 * @param[in]  name        Human-readable thread name.  May be truncated.
 * @param[in]  entry       Thread entry-point function.  Must not be NULL.
 * @param[in]  arg         Opaque argument forwarded to @p entry.  May be NULL.
 * @param[in]  priority    Scheduling priority.
 * @param[in]  stack_size  Stack size in bytes.  Must be > 0.
 * @param[in]  stack       Pointer to caller-allocated stack buffer.  Must be
 *                         8-byte aligned (ARM AAPCS).  Use the
 *                         @c OVE_THREAD_STACK_DEFINE_ family of helpers in
 *                         @c include/ove/storage.h.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_thread_deinit, ove_thread_create
 */
int ove_thread_init(ove_thread_t *handle, ove_thread_storage_t *storage, const char *name,
		    ove_thread_fn entry, void *arg, ove_prio_t priority, size_t stack_size,
		    void *stack);

/**
 * @brief Join and release a thread created with ove_thread_init().
 *
 * Waits for the entry function to return, then releases backend-internal
 * resources.  This function neither requests cooperative stop nor forcibly
 * terminates the worker.  Signal a cooperative worker with
 * ove_thread_request_stop() before calling it.  The static storage supplied at
 * init time is not freed.
 *
 * @param[in] handle  Handle returned by ove_thread_init().
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_thread_init
 */
int ove_thread_deinit(ove_thread_t handle);

/* _create / _destroy — heap-gated */
#ifdef OVE_HEAP_THREAD

/**
 * @brief Allocate and start a heap-backed thread.
 *
 * Both the backend storage and the stack are allocated from the RTOS heap.
 *
 * @note Requires @c OVE_HEAP_THREAD.  In zero-heap mode this function is
 *       not declared; use @c ove_thread_init() or
 *       @c OVE_THREAD_DEFINE_STATIC() instead.
 *
 * @param[out] handle      Receives the opaque thread handle on success.
 * @param[in]  name        Human-readable thread name.  May be truncated.
 * @param[in]  entry       Thread entry-point function.  Must not be NULL.
 * @param[in]  arg         Opaque argument forwarded to @p entry.  May be NULL.
 * @param[in]  priority    Scheduling priority.
 * @param[in]  stack_size  Stack size in bytes.  Must be > 0.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_thread_destroy
 */
int ove_thread_create(ove_thread_t *handle, const char *name, ove_thread_fn entry, void *arg,
		      ove_prio_t priority, size_t stack_size);

/**
 * @brief Join and free a thread created with ove_thread_create().
 *
 * @note Requires @c OVE_HEAP_THREAD.
 *
 * Waits for the entry function to return; it does not request stop or forcibly
 * terminate the worker.  Call ove_thread_request_stop() first when the entry
 * follows the cooperative stop contract.
 *
 * @param[in] handle  Handle returned by ove_thread_create().
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_thread_create
 */
int ove_thread_destroy(ove_thread_t handle);

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
 * @brief Cooperatively request that a thread stop running.
 *
 * Sets a per-thread sticky flag.  The worker must poll
 * @ref ove_thread_should_stop and return in response; the substrate does not
 * forcibly terminate it.  Safe from an ISR, another thread, or the worker
 * itself.
 *
 * @param[in] handle  Thread to signal.
 *
 * @see ove_thread_should_stop
 */
void ove_thread_request_stop(ove_thread_t handle);

/**
 * @brief Check whether the calling (or specified) thread has been asked to stop.
 *
 * Returns @c true if @ref ove_thread_request_stop was called for @p handle.
 * The flag is sticky: once set it stays set for the thread's lifetime.
 *
 * Workers should poll this in their main loop:
 * @code
 *   while (!ove_thread_should_stop(ove_thread_get_self())) {
 *       do_work();
 *   }
 * @endcode
 *
 * Safe to call from any context.
 *
 * @param[in] handle  Thread to inspect.
 * @return @c true if a stop has been requested, @c false otherwise.
 *
 * @see ove_thread_request_stop
 */
bool ove_thread_should_stop(ove_thread_t handle);

/**
 * @brief Query the minimum free stack observed for a thread.
 *
 * The returned headroom is the portion of the stack that remained unused at
 * the thread's deepest recorded stack usage.  A successful result of zero is
 * therefore distinct from a backend which cannot measure stack usage.
 *
 * @param[in]  handle         Thread to inspect. Must still be valid.
 * @param[out] headroom_bytes Receives the minimum free stack in bytes.
 * @return OVE_OK on success, OVE_ERR_NOT_SUPPORTED when stack profiling is
 *         unavailable, or OVE_ERR_INVALID_PARAM for an invalid argument.
 *
 * @see ove_thread_get_stack_usage
 */
int ove_thread_get_stack_headroom(ove_thread_t handle, size_t *headroom_bytes);

/**
 * @brief Query the minimum FREE stack a thread has ever had (its high-water usage margin).
 *
 * Despite the name, this returns the stack still UNUSED at the deepest point reached — the
 * remaining headroom — computed from the untouched fill pattern (FreeRTOS
 * uxTaskGetStackHighWaterMark; Zephyr/WASM sentinel scan). For bytes USED, subtract this from the
 * stack size. All backends agree on this free-not-used meaning; only the historical name says
 * "usage".
 *
 * @param[in] handle  Thread to inspect.
 * This compatibility API cannot distinguish unsupported profiling from a
 * genuinely exhausted stack. New code should use
 * @ref ove_thread_get_stack_headroom instead.
 *
 * @return Bytes of stack still free at the historical peak usage, or 0 on
 *         error or when the backend does not support stack profiling.
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
int ove_thread_get_runtime_stats(ove_thread_t handle, struct ove_thread_stats *stats);

/* ── System memory statistics ──────────────────────────────── */

/**
 * @brief System heap statistics.
 */
struct ove_mem_stats {
	size_t total;	  /**< Total heap size in bytes. */
	size_t free;	  /**< Current free heap in bytes. */
	size_t used;	  /**< Current used heap in bytes. */
	size_t peak_used; /**< High-water-mark usage in bytes. */
};

/**
 * @brief Query system heap statistics.
 *
 * @param[out] stats Caller-allocated structure to receive stats.
 * @return OVE_OK on success, OVE_ERR_NOT_SUPPORTED if unavailable.
 */
int ove_sys_get_mem_stats(struct ove_mem_stats *stats);

/* ── Thread enumeration ────────────────────────────────────── */

/**
 * @brief Cumulative time per thread state (microseconds).
 *
 * Only populated when CONFIG_OVE_THREAD_STATE_STATS is enabled.
 */
struct ove_thread_state_times {
	uint64_t running_us;   /**< Time in RUNNING state. */
	uint64_t ready_us;     /**< Time in READY state. */
	uint64_t blocked_us;   /**< Time in BLOCKED state. */
	uint64_t suspended_us; /**< Time in SUSPENDED state. */
};

/** @name Optional fields populated by @ref ove_thread_list
 * @{ */
#define OVE_THREAD_INFO_VALID_STACK_USED (1u << 0)
#define OVE_THREAD_INFO_VALID_STACK_SIZE (1u << 1)
#define OVE_THREAD_INFO_VALID_CPU_PERCENT (1u << 2)
#define OVE_THREAD_INFO_VALID_RUNNING_TIME (1u << 3)
#define OVE_THREAD_INFO_VALID_READY_TIME (1u << 4)
#define OVE_THREAD_INFO_VALID_BLOCKED_TIME (1u << 5)
#define OVE_THREAD_INFO_VALID_SUSPENDED_TIME (1u << 6)
#define OVE_THREAD_INFO_VALID_STATE_TIMES                                        \
	(OVE_THREAD_INFO_VALID_RUNNING_TIME | OVE_THREAD_INFO_VALID_READY_TIME | \
	 OVE_THREAD_INFO_VALID_BLOCKED_TIME | OVE_THREAD_INFO_VALID_SUSPENDED_TIME)
/** @} */

/**
 * @brief Snapshot of a single thread's info.
 */
struct ove_thread_info {
	const char *name;	   /**< Thread name (static, do not free). */
	uintptr_t identity;	   /**< Opaque native identity; equality only. */
	ove_thread_state_t state;  /**< Execution state. */
	int priority;		   /**< Priority level. */
	size_t stack_used;	   /**< Stack high-water mark; valid when its flag is set. */
	size_t stack_size;	   /**< Total stack allocation; valid when its flag is set. */
	uint32_t cpu_percent_x100; /**< CPU usage in 0.01% units (e.g. 1250 = 12.50%). */
	uint32_t valid_fields;	   /**< Bitwise OR of @c OVE_THREAD_INFO_VALID_* flags. */
	struct ove_thread_state_times state_times; /**< Per-state cumulative time. */
};

/**
 * @brief Test whether all requested optional snapshot fields are valid.
 */
static inline bool ove_thread_info_has(const struct ove_thread_info *info, uint32_t fields)
{
	return info != NULL && (info->valid_fields & fields) == fields;
}

/**
 * @brief List all threads in the system.
 *
 * @param[out] out          Array to fill with thread info.
 * @param[in]  max_count    Maximum entries in @p out.
 * @param[out] actual_count Number of entries written (may be NULL).
 * Optional metrics are valid only when their corresponding
 * @c OVE_THREAD_INFO_VALID_* bit is set. Their stored zero value is otherwise
 * just deterministic initialization, not a measurement.
 *
 * @return OVE_OK on success, OVE_ERR_QUEUE_FULL if entries were omitted,
 *         or OVE_ERR_NOT_SUPPORTED if enumeration is unavailable.
 */
int ove_thread_list(struct ove_thread_info *out, size_t max_count, size_t *actual_count);

#ifdef __cplusplus
}
#endif

#endif /* OVE_THREAD_H */

/** @} */
