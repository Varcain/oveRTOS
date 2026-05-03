/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_eventgroup Event Group
 * @ingroup ove_comm
 * @brief Bit-based event signaling between tasks and ISRs.
 *
 * An event group holds a set of binary flags (bits) that tasks can set,
 * clear, and wait on. Multiple tasks may block on the same event group
 * simultaneously, each specifying its own combination of bits and wait
 * semantics via the @c OVE_EG_WAIT_ALL and @c OVE_EG_CLEAR_ON_EXIT flags.
 *
 * Two allocation strategies are supported:
 * - @c _create() / @c _destroy() — heap-allocated.  Available only when
 *   @c OVE_HEAP_EVENTGROUP is defined (i.e. @c CONFIG_OVE_ZERO_HEAP is not set).
 * - @c _init() / @c _deinit() — caller-supplied storage.  Available in both
 *   modes.  See @c OVE_EVENTGROUP_DEFINE_STATIC for a one-step static helper.
 *
 * @note Requires @c CONFIG_OVE_EVENTGROUP.
 * @{
 */

#ifndef OVE_EVENTGROUP_H
#define OVE_EVENTGROUP_H

#include "ove/types.h"
#include "ove_config.h"
#include "ove/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wait flag: block until ALL requested bits are set simultaneously.
 *
 * When passed to @ref ove_eventgroup_wait_bits, the call blocks until every
 * bit in the @p bits mask is set at the same time. Without this flag the
 * call unblocks when ANY one of the requested bits becomes set.
 */
#define OVE_EG_WAIT_ALL 0x01

/**
 * @brief Wait flag: atomically clear the matched bits on return.
 *
 * When passed to @ref ove_eventgroup_wait_bits, the bits that satisfied the
 * wait condition are cleared atomically before the function returns. This
 * avoids a separate @ref ove_eventgroup_clear_bits call and prevents races
 * when multiple tasks share an event group.
 */
#define OVE_EG_CLEAR_ON_EXIT 0x02

#ifdef CONFIG_OVE_EVENTGROUP

/**
 * @brief Initialise an event group using caller-provided static storage.
 *
 * All bits in the new event group are cleared. The caller must ensure that
 * @p storage remains valid for the lifetime of the event group.
 *
 * @param[out] eg       Receives the initialised event group handle.
 * @param[in]  storage  Pointer to statically-allocated event group storage.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_EVENTGROUP.
 */
int ove_eventgroup_init(ove_eventgroup_t *eg, ove_eventgroup_storage_t *storage);

/**
 * @brief Deinitialise a statically-allocated event group.
 *
 * Releases all RTOS resources associated with @p eg. Any tasks still
 * blocked in @ref ove_eventgroup_wait_bits will be unblocked with an error.
 *
 * @param[in] eg  Event group handle returned by @ref ove_eventgroup_init.
 * @note Requires @c CONFIG_OVE_EVENTGROUP.
 */
void ove_eventgroup_deinit(ove_eventgroup_t eg);

/**
 * @brief Allocate and initialise a heap-backed event group.
 *
 * All bits in the new event group are cleared.
 *
 * @param[out] eg  Receives the created event group handle.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_EVENTGROUP and @c OVE_HEAP_EVENTGROUP.
 */
#ifdef OVE_HEAP_EVENTGROUP
int ove_eventgroup_create(ove_eventgroup_t *eg);

/**
 * @brief Destroy a heap-allocated event group.
 *
 * Frees the event group and all associated resources. Must only be called
 * on handles obtained from @ref ove_eventgroup_create.
 *
 * @param[in] eg  Event group handle returned by @ref ove_eventgroup_create.
 * @note Requires @c CONFIG_OVE_EVENTGROUP and @c OVE_HEAP_EVENTGROUP.
 */
void ove_eventgroup_destroy(ove_eventgroup_t eg);
#endif /* OVE_HEAP_EVENTGROUP */

/**
 * @brief Set one or more bits in the event group from task context.
 *
 * Atomically ORs @p bits into the current event group value. Any tasks
 * blocked in @ref ove_eventgroup_wait_bits whose wait conditions are now
 * satisfied will be unblocked.
 *
 * @param[in] eg    Event group handle.
 * @param[in] bits  Bitmask of bits to set.
 * @return The value of the event group immediately after the set operation,
 *         before any waiting tasks have had the chance to clear bits.
 * @note Must not be called from an ISR; use @ref ove_eventgroup_set_bits_from_isr.
 */
ove_eventbits_t ove_eventgroup_set_bits(ove_eventgroup_t eg, ove_eventbits_t bits);

/**
 * @brief Clear one or more bits in the event group.
 *
 * Atomically ANDs the complement of @p bits into the current event group
 * value. Clearing bits will not unblock any waiting tasks.
 *
 * @param[in] eg    Event group handle.
 * @param[in] bits  Bitmask of bits to clear.
 * @return The value of the event group immediately after the clear operation.
 */
ove_eventbits_t ove_eventgroup_clear_bits(ove_eventgroup_t eg, ove_eventbits_t bits);

/**
 * @brief Block until one or all of the requested bits are set.
 *
 * Waits for the bit pattern described by @p bits and @p flags. The behavior
 * is controlled by @p flags:
 * - @c OVE_EG_WAIT_ALL — require all bits in @p bits to be set simultaneously.
 * - @c OVE_EG_CLEAR_ON_EXIT — clear the matching bits atomically on return.
 *
 * On success the event bits at the time of the condition being met are
 * written to @p result.
 *
 * @param[in]  eg          Event group handle.
 * @param[in]  bits        Bitmask of bits to wait for.
 * @param[in]  flags       Combination of @c OVE_EG_WAIT_ALL and/or
 *                         @c OVE_EG_CLEAR_ON_EXIT; pass 0 for defaults.
 * @param[in]  timeout_ms  Maximum wait time in milliseconds; 0 for non-blocking.
 * @param[out] result      Receives the event bits value that satisfied the wait,
 *                         or @c NULL if not needed.
 * @return OVE_OK if the wait condition was met, @c OVE_ERR_TIMEOUT on timeout,
 *         negative error code on failure.
 */
int ove_eventgroup_wait_bits(ove_eventgroup_t eg, ove_eventbits_t bits, uint32_t flags,
			     uint32_t timeout_ms, ove_eventbits_t *result);

/**
 * @brief Set bits in the event group from an ISR.
 *
 * ISR-safe variant of @ref ove_eventgroup_set_bits. A context switch to a
 * higher-priority task that was unblocked may be requested by the underlying
 * RTOS after this call returns.
 *
 * @param[in] eg    Event group handle.
 * @param[in] bits  Bitmask of bits to set.
 * @return The value of the event group at the time of the call (before any
 *         pending context switch).
 */
ove_eventbits_t ove_eventgroup_set_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits);

/**
 * @brief Read the current bit value of the event group without blocking.
 *
 * Returns a snapshot of the event group's bit pattern at the time of the
 * call. The value may change immediately after the call returns.
 *
 * @param[in] eg  Event group handle.
 * @return Current event bits value.
 */
ove_eventbits_t ove_eventgroup_get_bits(ove_eventgroup_t eg);

#else /* !CONFIG_OVE_EVENTGROUP */

static inline int ove_eventgroup_create(ove_eventgroup_t *eg)
{
	(void)eg;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_eventgroup_destroy(ove_eventgroup_t eg)
{
	(void)eg;
}
static inline ove_eventbits_t ove_eventgroup_set_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	(void)eg;
	(void)bits;
	return 0;
}
static inline ove_eventbits_t ove_eventgroup_clear_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	(void)eg;
	(void)bits;
	return 0;
}
static inline int ove_eventgroup_wait_bits(ove_eventgroup_t eg, ove_eventbits_t bits,
					   uint32_t flags, uint32_t timeout_ms,
					   ove_eventbits_t *result)
{
	(void)eg;
	(void)bits;
	(void)flags;
	(void)timeout_ms;
	(void)result;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline ove_eventbits_t ove_eventgroup_set_bits_from_isr(ove_eventgroup_t eg,
							       ove_eventbits_t bits)
{
	(void)eg;
	(void)bits;
	return 0;
}
static inline ove_eventbits_t ove_eventgroup_get_bits(ove_eventgroup_t eg)
{
	(void)eg;
	return 0;
}

#endif /* CONFIG_OVE_EVENTGROUP */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_eventgroup group */

#endif /* OVE_EVENTGROUP_H */
