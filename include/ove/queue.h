/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file queue.h
 * @defgroup ove_queue Message queue
 * @brief Fixed-size item FIFO queue for inter-thread and ISR-to-thread
 *        communication.
 *
 * @note All functions in this group require @c CONFIG_OVE_QUEUE to be defined.
 *       When @c CONFIG_OVE_QUEUE is not set, every function is replaced by a
 *       static inline stub that returns @c OVE_ERR_NOT_SUPPORTED.
 *
 * Two allocation strategies are available:
 *  - @c _create() / @c _destroy() — heap-allocated.  Available only when
 *    @c OVE_HEAP_QUEUE is defined (i.e. @c CONFIG_OVE_ZERO_HEAP is not set).
 *  - @c _init() / @c _deinit() — caller-supplied storage and data buffer.
 *    Available in both modes.  See @c OVE_QUEUE_DEFINE_STATIC for a
 *    one-step static-allocation helper.
 * @{
 */

#ifndef OVE_QUEUE_H
#define OVE_QUEUE_H

#include "ove/types.h"
#include "ove_config.h"
#include "ove/time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle for a message queue object. */
typedef struct ove_queue *ove_queue_t;

#include "ove/storage.h"

#ifdef CONFIG_OVE_QUEUE

/**
 * @brief Initialise a queue using caller-supplied static storage and data buffer.
 *
 * No heap allocation is performed.  The @p buffer must be at least
 * @p item_size * @p max_items bytes and remain valid for the lifetime of
 * the queue.
 *
 * @note Requires @c CONFIG_OVE_QUEUE.
 *
 * @param[out] q          Receives the opaque queue handle on success.
 * @param[in]  storage    Pointer to statically allocated backend storage.
 *                        Must remain valid for the lifetime of the queue.
 * @param[in]  buffer     Caller-allocated data buffer of at least
 *                        @p item_size * @p max_items bytes.
 * @param[in]  item_size  Size in bytes of each queue item.  Must be > 0.
 * @param[in]  max_items  Maximum number of items the queue can hold.
 *                        Must be > 0.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_queue_deinit, ove_queue_create
 */
OVE_NODISCARD int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *storage, void *buffer,
				 size_t item_size, unsigned int max_items) OVE_NONNULL(1, 2, 3);

/**
 * @brief Release resources held by a queue initialised with ove_queue_init().
 *
 * The static storage and data buffer supplied at init time are not freed.
 *
 * @note Requires @c CONFIG_OVE_QUEUE.
 *
 * @param[in] q  Handle returned by ove_queue_init().
 *
 * @see ove_queue_init
 */
void ove_queue_deinit(ove_queue_t q);

/* _create / _destroy — heap-gated */
#ifdef OVE_HEAP_QUEUE

/**
 * @brief Allocate and initialise a queue from the heap.
 *
 * Both the backend storage and the item data buffer are allocated from
 * the heap.
 *
 * @note Requires @c CONFIG_OVE_QUEUE and @c OVE_HEAP_QUEUE
 *       (i.e. @c CONFIG_OVE_ZERO_HEAP must not be set).
 *
 * @param[out] q          Receives the opaque queue handle on success.
 * @param[in]  item_size  Size in bytes of each queue item.  Must be > 0.
 * @param[in]  max_items  Maximum number of items the queue can hold.
 *                        Must be > 0.
 * @return OVE_OK on success, or a negative error code on failure.
 *
 * @see ove_queue_destroy, ove_queue_init
 */
OVE_NODISCARD int ove_queue_create(ove_queue_t *q, size_t item_size, unsigned int max_items)
	OVE_NONNULL(1);

/**
 * @brief Destroy and free a queue allocated with ove_queue_create().
 *
 * @note Requires @c CONFIG_OVE_QUEUE and @c OVE_HEAP_QUEUE.
 *
 * @param[in] q  Handle returned by ove_queue_create().
 *
 * @see ove_queue_create
 */
void ove_queue_destroy(ove_queue_t q);

#endif /* OVE_HEAP_QUEUE */

/**
 * @brief Send an item to the back of the queue, blocking if it is full.
 *
 * Copies @p item_size bytes from @p data into the queue.  If the queue
 * is full, the caller blocks for up to @p timeout_ns nanoseconds.
 *
 * @note Must not be called from an ISR — use ove_queue_send_from_isr() instead.
 * @note Requires @c CONFIG_OVE_QUEUE.
 *
 * @param[in] q           Queue handle.
 * @param[in] data        Pointer to the item to copy into the queue.
 * @param[in] timeout_ns  Maximum time to wait in nanoseconds if the queue
 *                        is full.  Pass @c OVE_WAIT_FOREVER to block
 *                        indefinitely.  Use @c OVE_MS(n) / @c OVE_SEC(n)
 *                        helpers for ergonomic values.
 * @return OVE_OK on success, @c OVE_ERR_TIMEOUT if the queue remained full
 *         for the entire wait period, @c OVE_ERR_QUEUE_FULL if the queue is
 *         full and the timeout is zero, or another negative error code.
 *
 * @see ove_queue_receive, ove_queue_send_from_isr
 */
OVE_NODISCARD int ove_queue_send(ove_queue_t q, const void *data, uint64_t timeout_ns)
	OVE_NONNULL(1, 2);

/**
 * @brief Deadline-based variant of @ref ove_queue_send.
 *
 * Equivalent to calling @ref ove_queue_send with the time remaining until
 * @p deadline_ns (a steady-clock value from @ref ove_time_now_steady_ns).
 * Pass @c OVE_WAIT_FOREVER for an indefinite block.
 *
 * @note Requires @c CONFIG_OVE_QUEUE.
 *
 * @param[in] q           Queue handle.
 * @param[in] data        Pointer to the item to copy into the queue.
 * @param[in] deadline_ns Absolute deadline against @ref ove_time_now_steady_ns,
 *                        or @c OVE_WAIT_FOREVER.
 * @return Same set of return codes as @ref ove_queue_send.
 */
OVE_NODISCARD static inline int ove_queue_send_until(ove_queue_t q, const void *data,
						     uint64_t deadline_ns)
{
	return ove_queue_send(q, data, ove_time_deadline_to_timeout_ns(deadline_ns));
}

/**
 * @brief Receive (remove) an item from the front of the queue, blocking if
 *        it is empty.
 *
 * Copies @p item_size bytes out of the queue into @p buf.  If the queue is
 * empty, the caller blocks for up to @p timeout_ns nanoseconds.
 *
 * @note Must not be called from an ISR — use ove_queue_receive_from_isr()
 *       instead.
 * @note Requires @c CONFIG_OVE_QUEUE.
 *
 * @param[in]  q           Queue handle.
 * @param[out] buf         Buffer to copy the received item into.  Must be at
 *                         least @p item_size bytes.
 * @param[in]  timeout_ns  Maximum time to wait in nanoseconds if the queue
 *                         is empty.  Pass @c OVE_WAIT_FOREVER to block
 *                         indefinitely.  Use @c OVE_MS(n) / @c OVE_SEC(n)
 *                         helpers for ergonomic values.
 * @return OVE_OK on success, @c OVE_ERR_TIMEOUT if the queue remained empty
 *         for the entire wait period, @c OVE_ERR_QUEUE_EMPTY if the queue is
 *         empty and the timeout is zero, or another negative error code.
 *
 * @see ove_queue_send, ove_queue_receive_from_isr
 */
OVE_NODISCARD int ove_queue_receive(ove_queue_t q, void *buf, uint64_t timeout_ns)
	OVE_NONNULL(1, 2);

/**
 * @brief Deadline-based variant of @ref ove_queue_receive.
 *
 * Equivalent to calling @ref ove_queue_receive with the time remaining
 * until @p deadline_ns (a steady-clock value from
 * @ref ove_time_now_steady_ns).  Pass @c OVE_WAIT_FOREVER for an
 * indefinite block.
 *
 * @note Requires @c CONFIG_OVE_QUEUE.
 *
 * @param[in]  q           Queue handle.
 * @param[out] buf         Buffer to receive the item; must be at least
 *                         @c item_size bytes.
 * @param[in]  deadline_ns Absolute deadline against @ref ove_time_now_steady_ns,
 *                         or @c OVE_WAIT_FOREVER.
 * @return Same set of return codes as @ref ove_queue_receive.
 */
OVE_NODISCARD static inline int ove_queue_receive_until(ove_queue_t q, void *buf,
							uint64_t deadline_ns)
{
	return ove_queue_receive(q, buf, ove_time_deadline_to_timeout_ns(deadline_ns));
}

/**
 * @brief Send an item to the queue from an interrupt service routine.
 *
 * Non-blocking ISR-safe variant of ove_queue_send().  Returns immediately
 * if the queue is full.
 *
 * @note Requires @c CONFIG_OVE_QUEUE.
 *
 * @param[in] q     Queue handle.
 * @param[in] data  Pointer to the item to copy into the queue.
 * @return OVE_OK on success, @c OVE_ERR_QUEUE_FULL if the queue has no
 *         space, or another negative error code.
 *
 * @see ove_queue_send
 */
OVE_NODISCARD int ove_queue_send_from_isr(ove_queue_t q, const void *data) OVE_NONNULL(1, 2);

/**
 * @brief Receive an item from the queue from an interrupt service routine.
 *
 * Non-blocking ISR-safe variant of ove_queue_receive().  Returns immediately
 * if the queue is empty.
 *
 * @note Requires @c CONFIG_OVE_QUEUE.
 *
 * @param[in]  q    Queue handle.
 * @param[out] buf  Buffer to copy the received item into.  Must be at
 *                  least @p item_size bytes.
 * @return OVE_OK on success, @c OVE_ERR_QUEUE_EMPTY if the queue is empty,
 *         or another negative error code.
 *
 * @see ove_queue_receive
 */
OVE_NODISCARD int ove_queue_receive_from_isr(ove_queue_t q, void *buf) OVE_NONNULL(1, 2);

/**
 * @brief Register a notify callback fired after every successful send.
 *
 * The callback is invoked at the tail of @ref ove_queue_send and
 * @ref ove_queue_send_from_isr — once an item has been enqueued. Only
 * one slot per queue; a later call replaces an earlier registration.
 * Pass @c cb=NULL to clear.
 *
 * Designed for async runtimes that need a wake hook (the Rust binding
 * uses it to call @c AtomicWaker::wake on a task awaiting
 * @c AsyncQueue::recv).
 *
 * The callback runs in whatever context the originating send used —
 * thread or ISR. Implementations must be non-blocking and ISR-safe.
 *
 * @param[in] q          Queue handle.
 * @param[in] cb         Callback to invoke after successful sends, or
 *                       @c NULL to clear.
 * @param[in] user_data  Opaque pointer forwarded to @p cb.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_queue_set_notify(ove_queue_t q, ove_notify_cb cb, void *user_data) OVE_NONNULL(1);

#else /* !CONFIG_OVE_QUEUE */

/* P0-3: _init/_deinit stubs so OVE_QUEUE_DEFINE_STATIC links cleanly
 * when CONFIG_OVE_QUEUE=n. */
static inline int ove_queue_init(ove_queue_t *q, ove_queue_storage_t *s, void *b, size_t is,
				 unsigned int mi)
{
	(void)q;
	(void)s;
	(void)b;
	(void)is;
	(void)mi;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_queue_deinit(ove_queue_t q)
{
	(void)q;
}

static inline int ove_queue_create(ove_queue_t *q, size_t is, unsigned int mi)
{
	(void)q;
	(void)is;
	(void)mi;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_queue_destroy(ove_queue_t q)
{
	(void)q;
}
static inline int ove_queue_send(ove_queue_t q, const void *d, uint64_t t)
{
	(void)q;
	(void)d;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_queue_receive(ove_queue_t q, void *b, uint64_t t)
{
	(void)q;
	(void)b;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_queue_send_from_isr(ove_queue_t q, const void *d)
{
	(void)q;
	(void)d;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_queue_receive_from_isr(ove_queue_t q, void *b)
{
	(void)q;
	(void)b;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_queue_set_notify(ove_queue_t q, ove_notify_cb cb, void *ud)
{
	(void)q;
	(void)cb;
	(void)ud;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_QUEUE */

#ifdef __cplusplus
}
#endif

#endif /* OVE_QUEUE_H */

/** @} */
