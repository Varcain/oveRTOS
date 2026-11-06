/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file irq.h
 * @defgroup ove_irq Interrupt context
 * @ingroup ove_core
 * @brief Critical sections and interrupt-context detection.
 *
 * Interrupt-context detection supports synchronous bindings as well as
 * async dispatch. Critical sections support the async runtime's exclusion
 * provider (Embassy on Rust).
 *
 * @note Critical sections require @c CONFIG_OVE_ASYNC. The context query
 *       requires @c CONFIG_OVE_SYNC or @c CONFIG_OVE_ASYNC. Disabled functions
 *       use inert inline stubs; the disabled context query returns false.
 * @{
 */

#ifndef OVE_IRQ_H
#define OVE_IRQ_H

#include "ove/types.h"
#include "ove_config.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque cookie returned by @ref ove_irq_lock for use with
 *        @ref ove_irq_unlock.
 *
 * The width is sized to fit each backend's native restore state:
 *  - Zephyr:  @c unsigned @c int returned by @c irq_lock().
 *  - FreeRTOS: 0 (the FreeRTOS @c taskENTER_CRITICAL macros are
 *              symmetric and don't return a value).
 *  - NuttX:   @c irqstate_t (typically @c uint32_t).
 *  - host test backend: @c uint64_t (a TLS nesting depth — no real
 *    ISR-mask).
 *
 * The C ABI commits to @c uint64_t so the Rust binding's
 * `critical-section::Impl::RawRestoreState` can be a single fixed
 * size across every target.
 */
typedef uint64_t ove_irq_key_t;

#ifdef CONFIG_OVE_ASYNC

/**
 * @brief Enter a critical section: disable interrupts (or equivalent
 *        backend mechanism) and return an opaque restore cookie.
 *
 * Safe to call from both thread and ISR context. Nestable — each
 * @ref ove_irq_lock must be paired with a matching @ref ove_irq_unlock
 * passing the same cookie back, in LIFO order. The outermost
 * @ref ove_irq_unlock restores the previous interrupt state.
 *
 * @return Opaque cookie to pass to @ref ove_irq_unlock.
 * @note Requires @c CONFIG_OVE_ASYNC.
 */
ove_irq_key_t ove_irq_lock(void);

/**
 * @brief Leave a critical section, restoring the interrupt state
 *        captured by the corresponding @ref ove_irq_lock.
 *
 * @param[in] key  Cookie returned by the matching @ref ove_irq_lock.
 * @note Requires @c CONFIG_OVE_ASYNC.
 */
void ove_irq_unlock(ove_irq_key_t key);

#else /* !CONFIG_OVE_ASYNC */

static inline ove_irq_key_t ove_irq_lock(void)
{
	return 0;
}
static inline void ove_irq_unlock(ove_irq_key_t key)
{
	(void)key;
}

#endif /* CONFIG_OVE_ASYNC */

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_ASYNC)

/**
 * @brief Return true if the caller is currently in interrupt context.
 *
 * Used by bindings to check synchronous call context and dispatch between
 * thread-context and ISR-context wake operations. Requires no object setup
 * or allocation.
 *
 * @return @c true if the caller is in an ISR or equivalent
 *         interrupt-handling context, @c false otherwise.
 * @note On the host test backend the ISR wrappers set a
 *       thread-local flag; outside those
 *       paths the function returns @c false.
 * @note Requires @c CONFIG_OVE_SYNC or @c CONFIG_OVE_ASYNC.
 */
bool ove_is_in_isr(void);

#else /* Neither SYNC nor ASYNC */
static inline bool ove_is_in_isr(void)
{
	return false;
}

#endif /* CONFIG_OVE_SYNC || CONFIG_OVE_ASYNC */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_irq group */

#endif /* OVE_IRQ_H */
