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
 * @note Critical sections require @c CONFIG_OVE_ASYNC. Interrupt detection
 *       requires SYNC, ASYNC, QUEUE or EVENTGROUP; resource flags require SYNC,
 *       QUEUE or EVENTGROUP.
 *       Disabled functions use inert inline stubs and return false or zero.
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

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_ASYNC) || defined(CONFIG_OVE_QUEUE) || \
	defined(CONFIG_OVE_EVENTGROUP)

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
 * @note Requires @c CONFIG_OVE_SYNC, @c CONFIG_OVE_ASYNC, @c CONFIG_OVE_QUEUE
 *       or @c CONFIG_OVE_EVENTGROUP.
 */
bool ove_is_in_isr(void);

#else /* No SYNC, ASYNC, QUEUE or EVENTGROUP */
static inline bool ove_is_in_isr(void)
{
	return false;
}

#endif /* CONFIG_OVE_SYNC || CONFIG_OVE_ASYNC || CONFIG_OVE_QUEUE || CONFIG_OVE_EVENTGROUP */

/** Native resource initialization/destruction is available in this context. */
#define OVE_SYNC_CONTEXT_RESOURCE (1u << 0)
/** Native mutex acquisitions and condition-variable operations are available. */
#define OVE_SYNC_CONTEXT_BLOCKING (1u << 1)
/** Task semaphore/event posts and serialized notification setup are available. */
#define OVE_SYNC_CONTEXT_PRODUCER (1u << 2)

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_QUEUE) || defined(CONFIG_OVE_EVENTGROUP)
/**
 * @brief Query synchronization context without touching a primitive.
 *
 * Returns a mask of @c OVE_SYNC_CONTEXT_RESOURCE,
 * @c OVE_SYNC_CONTEXT_BLOCKING and @c OVE_SYNC_CONTEXT_PRODUCER.
 * A missing bit means the binding must reject that operation or retain a
 * resource whose destruction is unavailable.
 * Object validity, ownership and quiescence are separate requirements.
 *
 * FreeRTOS permits resource setup and task posts before scheduler start;
 * blocking requires a running, non-idle task with interrupts enabled.
 * Both scheduler-state and
 * idle-task queries must be enabled in its configuration. Zephyr and NuttX
 * require an active kernel and non-idle thread. Resource and blocking admission
 * additionally require enabled interrupts. Task producers remain available in
 * recognized masked contexts, including nested native critical sections.
 * FreeRTOS also permits task producers while its scheduler is suspended.
 * Cooperative and scheduler-locked threads remain supported where native
 * blocking is legal. The POSIX backend reports producer-only admission inside
 * its modeled critical sections. All providers return zero in ISR context;
 * native ISR producer entries have separate kernel-aware interrupt contracts.
 * Producer admission does not permit mutex acquisition or arbitrary nonblocking
 * operations, and does not serialize notification registration with gives.
 * Unsupported architecture/configuration domains return zero conservatively.
 * Current native domains are privileged Cortex-M3/M4/M7, Zephyr native_sim,
 * single-core NuttX flat sim, and the POSIX backend. FreeRTOS multicore and
 * NuttX SMP return zero.
 * The query allocates nothing, changes
 * no interrupt mask and requires no TIME, ASYNC or thread-wrapper feature.
 *
 * This is not the eligibility test for releasing an already-owned mutex:
 * a valid same-thread unlock can remain legal with interrupts masked.
 *
 * @return Context flags, or zero when SYNC, QUEUE and EVENTGROUP are all
 *         disabled or the context is unavailable.
 */
uint32_t ove_sync_context(void);
#else
static inline uint32_t ove_sync_context(void)
{
	return 0;
}
#endif

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_irq group */

#endif /* OVE_IRQ_H */
