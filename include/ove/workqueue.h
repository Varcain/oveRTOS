/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_workqueue Work Queue
 * @ingroup ove_comm
 * @brief Deferred work execution on a dedicated RTOS thread.
 *
 * A work queue owns a single thread that executes submitted work items in
 * FIFO order. Work items may be submitted immediately or after a delay,
 * and pending items may be cancelled before execution begins.
 *
 * Two allocation strategies are supported for the queue itself:
 * - @c _create() / @c _destroy() — unified API that works in both heap and
 *   zero-heap mode.  In zero-heap mode these are macros that generate
 *   per-call-site static storage; the stack size must be a compile-time
 *   constant.
 * - @c _init() / @c _deinit() — explicit storage control with caller-supplied
 *   buffers.  Use when creating objects in loops, arrays, or structs.
 *
 * Work items similarly have static (@ref ove_work_init_static) and heap
 * (@ref ove_work_init / @ref ove_work_free) variants.
 *
 * @note Requires @c CONFIG_OVE_WORKQUEUE.
 * @{
 */

#ifndef OVE_WORKQUEUE_H
#define OVE_WORKQUEUE_H

#include "ove/types.h"
#include "ove/thread.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Prototype for a work item handler function.
 *
 * Called by the work queue thread when the work item is executed. The
 * @p work handle may be used to reschedule or identify the item inside
 * the handler.
 *
 * @param[in] work  Handle of the work item being executed.
 */
typedef void (*ove_work_fn)(ove_work_t work);

#include "ove/storage.h"

#ifdef CONFIG_OVE_WORKQUEUE

/**
 * @brief Initialise a work queue using caller-provided static storage.
 *
 * Creates the underlying RTOS thread with the given @p name, @p priority,
 * @p stack_size, and pre-allocated @p stack buffer. The queue begins
 * dispatching items as soon as the first work item is submitted.
 *
 * @param[out] wq          Receives the initialised work queue handle.
 * @param[in]  storage     Pointer to statically-allocated work queue storage.
 * @param[in]  name        Human-readable name for the underlying thread.
 * @param[in]  priority    Thread priority for the work queue thread.
 * @param[in]  stack_size  Size of the thread stack in bytes.
 * @param[in]  stack       Pointer to the pre-allocated stack buffer.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WORKQUEUE.
 */
int  ove_workqueue_init(ove_workqueue_t *wq,
			ove_workqueue_storage_t *storage,
			const char *name, ove_prio_t priority,
			size_t stack_size, void *stack);

/**
 * @brief Deinitialise a statically-allocated work queue.
 *
 * Stops the underlying thread and releases all RTOS resources. Any work
 * items still in the queue at deinit time are discarded without execution.
 *
 * @param[in] wq  Work queue handle returned by @ref ove_workqueue_init.
 * @note Requires @c CONFIG_OVE_WORKQUEUE.
 */
void ove_workqueue_deinit(ove_workqueue_t wq);

/**
 * @brief Initialise a work item using caller-provided static storage.
 *
 * Associates the @p handler function with the work item. The item must be
 * initialised before it can be submitted to a work queue.
 *
 * @param[out] work     Receives the initialised work handle.
 * @param[in]  storage  Pointer to statically-allocated work item storage.
 * @param[in]  handler  Function to call when the work item is executed.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WORKQUEUE.
 */
int  ove_work_init_static(ove_work_t *work,
			  ove_work_storage_t *storage,
			  ove_work_fn handler);

#ifndef CONFIG_OVE_ZERO_HEAP
/**
 * @brief Allocate and initialise a heap-backed work item.
 *
 * Allocates the work item control structure from the heap and associates
 * @p handler with it.
 *
 * @param[out] work     Receives the created work handle.
 * @param[in]  handler  Function to call when the work item is executed.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WORKQUEUE and that @c CONFIG_OVE_ZERO_HEAP
 *       is not set.
 */
int  ove_work_init(ove_work_t *work, ove_work_fn handler);

/**
 * @brief Free a heap-allocated work item.
 *
 * Releases the memory allocated by @ref ove_work_init. The item must not
 * be pending in a work queue when this function is called; cancel it first
 * with @ref ove_work_cancel if necessary.
 *
 * @param[in] work  Work handle returned by @ref ove_work_init.
 * @note Requires @c CONFIG_OVE_WORKQUEUE and that @c CONFIG_OVE_ZERO_HEAP
 *       is not set.
 */
void ove_work_free(ove_work_t work);
#endif

/**
 * @brief Allocate a heap-backed work queue.
 *
 * Allocates the work queue and its thread from the heap and starts
 * dispatching immediately.
 *
 * @param[out] wq          Receives the created work queue handle.
 * @param[in]  name        Human-readable name for the underlying thread.
 * @param[in]  priority    Thread priority for the work queue thread.
 * @param[in]  stack_size  Stack size in bytes for the work queue thread.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WORKQUEUE and @c OVE_HEAP_WORKQUEUE.
 */
#ifdef OVE_HEAP_WORKQUEUE
int  ove_workqueue_create(ove_workqueue_t *wq, const char *name,
			  ove_prio_t priority, size_t stack_size);

/**
 * @brief Destroy a heap-allocated work queue.
 *
 * Stops the underlying thread and frees all resources. Pending work items
 * are discarded. Must only be called on handles from @ref ove_workqueue_create.
 *
 * @param[in] wq  Work queue handle returned by @ref ove_workqueue_create.
 * @note Requires @c CONFIG_OVE_WORKQUEUE and @c OVE_HEAP_WORKQUEUE.
 */
void ove_workqueue_destroy(ove_workqueue_t wq);
#elif !defined(__ZIG_CIMPORT__) /* !OVE_HEAP_WORKQUEUE — zero-heap mode */

/* Unified macro — stack_size must be a compile-time constant. */
#define ove_workqueue_create(pwq, name, priority, stack_size) \
	({ static ove_workqueue_storage_t _ove_stor_; \
	   OVE_THREAD_STACK_BLOCK_STATIC_(_ove_stk_, (stack_size)); \
	   ove_workqueue_init((pwq), &_ove_stor_, (name), (priority), \
			      (stack_size), _ove_stk_); })
#define ove_workqueue_destroy(wq) ove_workqueue_deinit(wq)

#endif /* OVE_HEAP_WORKQUEUE */

/**
 * @brief Submit a work item for immediate execution on the work queue.
 *
 * Enqueues @p work for execution on @p wq. If the item is already pending
 * the call may fail or reset the pending state depending on the underlying
 * implementation.
 *
 * @param[in] wq    Target work queue handle.
 * @param[in] work  Work item handle to submit.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_work_submit(ove_workqueue_t wq, ove_work_t work);

/**
 * @brief Submit a work item for execution after a delay.
 *
 * Schedules @p work to run on @p wq after at least @p delay_ms milliseconds
 * have elapsed. The item may be cancelled before the delay expires using
 * @ref ove_work_cancel.
 *
 * @param[in] wq        Target work queue handle.
 * @param[in] work      Work item handle to schedule.
 * @param[in] delay_ms  Minimum delay before execution in milliseconds.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_work_submit_delayed(ove_workqueue_t wq,
			     ove_work_t work, uint32_t delay_ms);

/**
 * @brief Cancel a pending work item before it executes.
 *
 * Attempts to remove @p work from the queue before its handler is called.
 * Has no effect if the item is not pending or is already executing.
 *
 * @param[in] work  Work item handle to cancel.
 * @return OVE_OK if the item was successfully cancelled, @c OVE_ERR_INVAL
 *         if the item was not pending, or another negative error code on failure.
 */
int  ove_work_cancel(ove_work_t work);

#else /* !CONFIG_OVE_WORKQUEUE */

static inline int  ove_workqueue_create(ove_workqueue_t *wq, const char *n, ove_prio_t p, size_t s) { (void)wq; (void)n; (void)p; (void)s; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_workqueue_destroy(ove_workqueue_t wq) { (void)wq; }
static inline int  ove_work_init(ove_work_t *w, ove_work_fn h) { (void)w; (void)h; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_work_free(ove_work_t w) { (void)w; }
static inline int  ove_work_submit(ove_workqueue_t wq, ove_work_t w) { (void)wq; (void)w; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t w, uint32_t d) { (void)wq; (void)w; (void)d; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_work_cancel(ove_work_t w) { (void)w; return OVE_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_OVE_WORKQUEUE */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_workqueue group */

#endif /* OVE_WORKQUEUE_H */
