/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file stream.h
 * @defgroup ove_stream Stream
 * @ingroup ove_comm
 * @brief Byte stream communication primitives.
 *
 * Provides a FIFO byte-stream channel suitable for producer/consumer
 * communication between tasks or between an ISR and a task. Streams are
 * trigger-aware: a receive unblocks only when at least @p trigger bytes are
 * available, enabling efficient framing without spin-waiting.
 *
 * Two allocation strategies are supported:
 * - @c _create() / @c _destroy() — heap-allocated.  Available only when
 *   @c OVE_HEAP_STREAM is defined (i.e. @c CONFIG_OVE_ZERO_HEAP is not set).
 * - @c _init() / @c _deinit() — caller-supplied storage and buffer.
 *   Available in both modes.  See @c OVE_STREAM_DEFINE_STATIC for a
 *   one-step static helper.
 *
 * @note Requires @c CONFIG_OVE_STREAM.
 * @{
 */

#ifndef OVE_STREAM_H
#define OVE_STREAM_H

#include "ove/types.h"
#include "ove_config.h"
#include "ove/storage.h"
#include "ove/time.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_STREAM

/**
 * @brief Initialise a stream using caller-provided static storage.
 *
 * Constructs a stream object in @p storage and associates the raw byte
 * buffer @p buffer of @p size bytes with it. The stream will not unblock
 * a waiting receiver until at least @p trigger bytes are present.
 *
 * @param[out] stream   Receives the initialised stream handle.
 * @param[in]  storage  Pointer to statically-allocated stream storage.
 * @param[in]  buffer   Pointer to the backing byte buffer.
 * @param[in]  size     Size of @p buffer in bytes.
 * @param[in]  trigger  Minimum bytes available before a blocked receiver wakes.
 *                      A value of 0 is treated as 1 by every backend.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_STREAM.
 */
int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger);

/**
 * @brief Deinitialise a statically-allocated stream.
 *
 * Releases all RTOS resources associated with @p stream. The caller is
 * responsible for freeing any backing buffer that was passed to
 * @ref ove_stream_init.
 *
 * @param[in] stream  Stream handle returned by @ref ove_stream_init.
 * @note Requires @c CONFIG_OVE_STREAM.
 */
void ove_stream_deinit(ove_stream_t stream);

/**
 * @brief Allocate and initialise a heap-backed stream.
 *
 * Allocates both the stream control structure and the internal byte buffer
 * from the heap. The stream will not wake a blocked receiver until at least
 * @p trigger bytes are available.
 *
 * @param[out] stream   Receives the created stream handle.
 * @param[in]  size     Capacity of the stream buffer in bytes.
 * @param[in]  trigger  Minimum bytes available before a blocked receiver wakes.
 *                      A value of 0 is treated as 1 by every backend.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_STREAM and @c OVE_HEAP_STREAM.
 */
#ifdef OVE_HEAP_STREAM
int ove_stream_create(ove_stream_t *stream, size_t size, size_t trigger);

/**
 * @brief Destroy a heap-allocated stream.
 *
 * Frees the stream control structure and its internal buffer. Must only be
 * called on handles obtained from @ref ove_stream_create.
 *
 * @param[in] stream  Stream handle returned by @ref ove_stream_create.
 * @note Requires @c CONFIG_OVE_STREAM and @c OVE_HEAP_STREAM.
 */
void ove_stream_destroy(ove_stream_t stream);
#endif /* OVE_HEAP_STREAM */

/**
 * @brief Send bytes into the stream from task context.
 *
 * Copies up to @p len bytes from @p data into the stream. Blocks for at most
 * @p timeout_ns nanoseconds if the stream has insufficient space. The actual
 * number of bytes accepted is written to @p bytes_sent when not @c NULL.
 *
 * @param[in]  stream      Stream handle.
 * @param[in]  data        Pointer to the data to send.
 * @param[in]  len         Number of bytes to send.
 * @param[in]  timeout_ns  Maximum wait time in nanoseconds; 0 for non-blocking.
 * @param[out] bytes_sent  Receives the number of bytes actually written, or
 *                         @c NULL if the caller does not need this value.
 * @return OVE_OK on success, negative error code on failure or timeout.
 * @note Must not be called from an ISR; use @ref ove_stream_send_from_isr instead.
 */
int ove_stream_send(ove_stream_t stream, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_sent);

/**
 * @brief Deadline-based variant of @ref ove_stream_send.
 *
 * Equivalent to calling @ref ove_stream_send with the time remaining
 * until @p deadline_ns (a steady-clock value from
 * @ref ove_time_now_steady_ns).  Pass @c OVE_WAIT_FOREVER for an
 * indefinite block.
 *
 * @param[in]  stream      Stream handle.
 * @param[in]  data        Pointer to the data to send.
 * @param[in]  len         Number of bytes to send.
 * @param[in]  deadline_ns Absolute deadline against @ref ove_time_now_steady_ns,
 *                         or @c OVE_WAIT_FOREVER.
 * @param[out] bytes_sent  Receives the number of bytes actually written, or
 *                         @c NULL.
 * @return OVE_OK on success, negative error code on failure.
 */
static inline int ove_stream_send_until(ove_stream_t stream, const void *data, size_t len,
					uint64_t deadline_ns, size_t *bytes_sent)
{
	return ove_stream_send(stream, data, len, ove_time_deadline_to_timeout_ns(deadline_ns),
			       bytes_sent);
}

/**
 * @brief Receive bytes from the stream in task context.
 *
 * Copies up to @p len bytes from the stream into @p buf. Blocks for at most
 * @p timeout_ns nanoseconds until the stream's trigger threshold is satisfied.
 * The actual number of bytes read is written to @p bytes_received when not
 * @c NULL.
 *
 * @param[in]  stream          Stream handle.
 * @param[out] buf             Buffer to receive data.
 * @param[in]  len             Maximum number of bytes to read.
 * @param[in]  timeout_ns      Maximum wait time in nanoseconds; 0 for non-blocking.
 * @param[out] bytes_received  Receives the number of bytes actually read, or
 *                             @c NULL if the caller does not need this value.
 * @return OVE_OK on success, negative error code on failure or timeout.
 * @note Must not be called from an ISR; use @ref ove_stream_receive_from_isr instead.
 */
int ove_stream_receive(ove_stream_t stream, void *buf, size_t len, uint64_t timeout_ns,
		       size_t *bytes_received);

/**
 * @brief Deadline-based variant of @ref ove_stream_receive.
 *
 * Equivalent to calling @ref ove_stream_receive with the time remaining
 * until @p deadline_ns (a steady-clock value from
 * @ref ove_time_now_steady_ns).  Pass @c OVE_WAIT_FOREVER for an
 * indefinite block.
 *
 * @param[in]  stream         Stream handle.
 * @param[out] buf            Buffer to receive data.
 * @param[in]  len            Maximum number of bytes to read.
 * @param[in]  deadline_ns    Absolute deadline against @ref ove_time_now_steady_ns,
 *                            or @c OVE_WAIT_FOREVER.
 * @param[out] bytes_received Receives the number of bytes actually read, or
 *                            @c NULL.
 * @return OVE_OK on success, negative error code on failure.
 */
static inline int ove_stream_receive_until(ove_stream_t stream, void *buf, size_t len,
					   uint64_t deadline_ns, size_t *bytes_received)
{
	return ove_stream_receive(stream, buf, len, ove_time_deadline_to_timeout_ns(deadline_ns),
				  bytes_received);
}

/**
 * @brief Send bytes into the stream from an ISR.
 *
 * ISR-safe variant of @ref ove_stream_send. Never blocks; if the stream has
 * insufficient space the call returns immediately with a partial or zero count.
 *
 * @param[in]  stream      Stream handle.
 * @param[in]  data        Pointer to the data to send.
 * @param[in]  len         Number of bytes to send.
 * @param[out] bytes_sent  Receives the number of bytes actually written, or
 *                         @c NULL if the caller does not need this value.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_stream_send_from_isr(ove_stream_t stream, const void *data, size_t len, size_t *bytes_sent);

/**
 * @brief Receive bytes from the stream from an ISR.
 *
 * ISR-safe variant of @ref ove_stream_receive. Never blocks; returns only
 * the bytes that are immediately available.
 *
 * @param[in]  stream          Stream handle.
 * @param[out] buf             Buffer to receive data.
 * @param[in]  len             Maximum number of bytes to read.
 * @param[out] bytes_received  Receives the number of bytes actually read, or
 *                             @c NULL if the caller does not need this value.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_stream_receive_from_isr(ove_stream_t stream, void *buf, size_t len, size_t *bytes_received);

/**
 * @brief Discard all bytes currently held in the stream.
 *
 * Resets the stream to the empty state without deallocating resources.
 * Any task blocked in @ref ove_stream_receive may remain blocked after this
 * call until new data is written.
 *
 * @param[in] stream  Stream handle.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_stream_reset(ove_stream_t stream);

/**
 * @brief Query the number of bytes currently available in the stream.
 *
 * Returns the count of bytes that can be read without blocking.
 *
 * @param[in] stream  Stream handle.
 * @return Number of bytes available; 0 if the stream is empty or invalid.
 */
size_t ove_stream_bytes_available(ove_stream_t stream);

/**
 * @brief Register a notify callback fired after every successful send.
 *
 * The callback is invoked at the tail of @ref ove_stream_send and
 * @ref ove_stream_send_from_isr — once data has actually been deposited.
 * Only one callback slot per stream; a later call replaces an earlier
 * registration. Pass @c cb=NULL to clear.
 *
 * Designed for higher-level async runtimes that need a wake hook:
 * the Rust binding registers a callback that calls
 * @c AtomicWaker::wake on a task suspended on @c Stream::recv_async.
 *
 * The callback runs in whatever context the originating send used
 * (thread or ISR).  Implementations must therefore be ISR-safe and
 * non-blocking — typically a single store + waker poke.
 *
 * @param[in] stream     Stream handle.
 * @param[in] cb         Callback to invoke after successful sends, or
 *                       @c NULL to clear.
 * @param[in] user_data  Opaque pointer forwarded to @p cb.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_stream_set_notify(ove_stream_t stream, ove_notify_cb cb, void *user_data);

#else /* !CONFIG_OVE_STREAM */

/* P0-3: _init/_deinit stubs so OVE_STREAM_DEFINE_STATIC links cleanly when
 * CONFIG_OVE_STREAM=n. */
static inline int ove_stream_init(ove_stream_t *s, ove_stream_storage_t *st, void *b, size_t sz,
				  size_t t)
{
	(void)s;
	(void)st;
	(void)b;
	(void)sz;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_stream_deinit(ove_stream_t s)
{
	(void)s;
}

static inline int ove_stream_create(ove_stream_t *s, size_t sz, size_t t)
{
	(void)s;
	(void)sz;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_stream_destroy(ove_stream_t s)
{
	(void)s;
}
static inline int ove_stream_send(ove_stream_t s, const void *d, size_t l, uint64_t t, size_t *bs)
{
	(void)s;
	(void)d;
	(void)l;
	(void)t;
	(void)bs;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_stream_receive(ove_stream_t s, void *b, size_t l, uint64_t t, size_t *br)
{
	(void)s;
	(void)b;
	(void)l;
	(void)t;
	(void)br;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_stream_send_from_isr(ove_stream_t s, const void *d, size_t l, size_t *bs)
{
	(void)s;
	(void)d;
	(void)l;
	(void)bs;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_stream_receive_from_isr(ove_stream_t s, void *b, size_t l, size_t *br)
{
	(void)s;
	(void)b;
	(void)l;
	(void)br;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_stream_reset(ove_stream_t s)
{
	(void)s;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline size_t ove_stream_bytes_available(ove_stream_t s)
{
	(void)s;
	return 0;
}
static inline int ove_stream_set_notify(ove_stream_t s, ove_notify_cb cb, void *ud)
{
	(void)s;
	(void)cb;
	(void)ud;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_STREAM */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_stream group */

#endif /* OVE_STREAM_H */
