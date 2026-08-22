/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file workqueue.h
 * @defgroup ove_workqueue Work Queue
 * @ingroup ove_comm
 * @brief Deferred work execution on a dedicated RTOS thread.
 *
 * A work queue owns a single thread that executes submitted work items in
 * FIFO order. Work items may be submitted immediately or after a delay,
 * and pending items may be cancelled before execution begins.
 *
 * Two allocation strategies are supported for the queue itself:
 * - @c _create() / @c _destroy() — heap-allocated, including thread stack.
 *   Available only when @c OVE_HEAP_WORKQUEUE is defined (i.e.
 *   @c CONFIG_OVE_ZERO_HEAP is not set).
 * - @c _init() / @c _deinit() — caller-supplied storage and stack buffer.
 *   Available in both modes.  See @c OVE_WORKQUEUE_DEFINE_STATIC for a
 *   one-step static helper.
 *
 * Work items have a static lifecycle pair @ref ove_work_init_static /
 * @ref ove_work_deinit (both modes) and a heap-only pair @ref ove_work_init /
 * @ref ove_work_free. Deinitialising or freeing an item synchronously drains
 * any queued, delayed, or running use of its storage.
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
 * @p work handle may be used to identify the item. Submitting the same item
 * from its handler is rejected as busy; recurring work needs another item,
 * a timer, or an external submitter.
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
int ove_workqueue_init(ove_workqueue_t *wq, ove_workqueue_storage_t *storage, const char *name,
		       ove_prio_t priority, size_t stack_size, void *stack);

/**
 * @brief Deinitialise a statically-allocated work queue.
 *
 * Stops the underlying thread and releases all RTOS resources. Deinitialise
 * every work item submitted to this queue before deinitialising the queue;
 * this explicit ordering also drains delayed and running submissions.
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
int ove_work_init_static(ove_work_t *work, ove_work_storage_t *storage, ove_work_fn handler);

/**
 * @brief Deinitialise a work item and release its RTOS resources.
 *
 * Synchronously cancels a queued or delayed submission and waits for a
 * handler that is already running. After return, the caller-provided storage
 * may be reused or leave scope. The target work queue must remain alive until
 * this function returns.
 *
 * @param[in] work  Work handle returned by @ref ove_work_init_static.
 * @note Call from thread context, never from the item's own handler or an ISR.
 */
void ove_work_deinit(ove_work_t work);

#ifdef OVE_HEAP_WORKQUEUE
/**
 * @brief Allocate and initialise a heap-backed work item.
 *
 * Allocates the work item control structure from the heap and associates
 * @p handler with it.
 *
 * @param[out] work     Receives the created work handle.
 * @param[in]  handler  Function to call when the work item is executed.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_WORKQUEUE and @c OVE_HEAP_WORKQUEUE.
 */
int ove_work_init(ove_work_t *work, ove_work_fn handler);

/**
 * @brief Free a heap-allocated work item.
 *
 * Synchronously drains the item and releases the memory allocated by
 * @ref ove_work_init. The target work queue must remain alive until this
 * function returns.
 *
 * @param[in] work  Work handle returned by @ref ove_work_init.
 * @note Requires @c CONFIG_OVE_WORKQUEUE and @c OVE_HEAP_WORKQUEUE.
 */
void ove_work_free(ove_work_t work);
#endif /* OVE_HEAP_WORKQUEUE */

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
int ove_workqueue_create(ove_workqueue_t *wq, const char *name, ove_prio_t priority,
			 size_t stack_size);

/**
 * @brief Destroy a heap-allocated work queue.
 *
 * Stops the underlying thread and frees all resources. Every submitted work
 * item must first be deinitialised or freed. Must only be called on handles
 * from @ref ove_workqueue_create.
 *
 * @param[in] wq  Work queue handle returned by @ref ove_workqueue_create.
 * @note Requires @c CONFIG_OVE_WORKQUEUE and @c OVE_HEAP_WORKQUEUE.
 */
void ove_workqueue_destroy(ove_workqueue_t wq);
#endif /* OVE_HEAP_WORKQUEUE */

/**
 * @brief Submit a work item for immediate execution on the work queue.
 *
 * Enqueues @p work for execution on @p wq. Returns @c OVE_ERR_BUSY when the
 * item is already queued, delayed, or running.
 *
 * @param[in] wq    Target work queue handle.
 * @param[in] work  Work item handle to submit.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_work_submit(ove_workqueue_t wq, ove_work_t work);

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
int ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t work, uint32_t delay_ms);

/**
 * @brief Cancel a pending work item before it executes.
 *
 * Synchronously removes @p work before its handler is called. If the handler
 * is already executing, waits for it to finish. After return, the work item's
 * storage is no longer referenced asynchronously and may be deinitialised.
 *
 * @param[in] work  Work item handle to cancel.
 * @return OVE_OK if the item was successfully cancelled, @c OVE_ERR_INVAL
 *         if the item was not pending, or another negative error code on failure.
 */
int ove_work_cancel(ove_work_t work);

#else /* !CONFIG_OVE_WORKQUEUE */

/* P0-3: _init/_deinit stubs so OVE_WORKQUEUE_DEFINE_STATIC links cleanly
 * when CONFIG_OVE_WORKQUEUE=n. */
static inline int ove_workqueue_init(ove_workqueue_t *wq, ove_workqueue_storage_t *st,
				     const char *n, ove_prio_t p, size_t s, void *stack)
{
	(void)wq;
	(void)st;
	(void)n;
	(void)p;
	(void)s;
	(void)stack;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_workqueue_deinit(ove_workqueue_t wq)
{
	(void)wq;
}

static inline int ove_workqueue_create(ove_workqueue_t *wq, const char *n, ove_prio_t p, size_t s)
{
	(void)wq;
	(void)n;
	(void)p;
	(void)s;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_workqueue_destroy(ove_workqueue_t wq)
{
	(void)wq;
}
static inline int ove_work_init(ove_work_t *w, ove_work_fn h)
{
	(void)w;
	(void)h;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_work_init_static(ove_work_t *w, ove_work_storage_t *s, ove_work_fn h)
{
	(void)w;
	(void)s;
	(void)h;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_work_deinit(ove_work_t w)
{
	(void)w;
}
static inline void ove_work_free(ove_work_t w)
{
	(void)w;
}
static inline int ove_work_submit(ove_workqueue_t wq, ove_work_t w)
{
	(void)wq;
	(void)w;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_work_submit_delayed(ove_workqueue_t wq, ove_work_t w, uint32_t d)
{
	(void)wq;
	(void)w;
	(void)d;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_work_cancel(ove_work_t w)
{
	(void)w;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_WORKQUEUE */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_workqueue group */

#endif /* OVE_WORKQUEUE_H */
