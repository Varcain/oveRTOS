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
 * @brief Opaque interrupt-state cookie returned by @ref ove_irq_lock.
 *
 * The fixed uint64_t ABI accommodates each backend's native restore state.
 * FreeRTOS retains its native ISR cookie and architectural mask state;
 * task cookies are not required to be zero. Callers must pass the entire
 * unchanged cookie to its matching unlock, without inspecting its encoding.
 */
typedef uint64_t ove_irq_key_t;

#ifdef CONFIG_OVE_ASYNC

/**
 * @brief Acquire the backend's native exclusion domain and capture its state.
 *
 * Nestable: pair each call exactly once with @ref ove_irq_unlock, passing the
 * same opaque cookie in LIFO order on the acquiring thread or handler activation.
 * Keep the body short and nonblocking: do not yield, await, start the scheduler,
 * migrate context, or change interrupt priorities/grouping or privilege while held.
 *
 * @par FreeRTOS domain and admission
 * Supported single-core Cortex-M3/M4/M7 ports exclude kernel-aware participants,
 * using configMAX_SYSCALL_INTERRUPT_PRIORITY. Higher-urgency interrupts remain
 * able to run and must not participate in this provider's shared-state protection.
 * Supported handlers are external IRQs, SysTick and PendSV at eligible priority
 * and grouping. Fault, NMI, supervisor, debug and reserved handlers are excluded.
 * Unsupported FreeRTOS ports/configurations/contexts stop without returning a key;
 * this enforcement remains active with assertions disabled. Priority-bit metadata
 * must match the hardware. Ordinary unprivileged tasks are unsupported.
 *
 * Restricted MPU V1 tasks require a permitted, serviceable priority-zero SVCall,
 * with PRIMASK and FAULTMASK clear. This is a native setup precondition, not a
 * condition that unprivileged mask reads can diagnose before attempting SVCall.
 * Supported calls preserve the caller's privilege before returning to the body.
 *
 * @par FreeRTOS restoration and native-call limitation
 * The wrapper preserves incoming exclusion during its own transitions and
 * restores captured PRIMASK, BASEPRI and FAULTMASK on matching unlock. Exceptional
 * transitions briefly mask faults; no whole-body fault mask or latency bound is
 * promised. Native operations inside the body can still lower stronger raw masks.
 * For example, with BASEPRI = 0x40 and native threshold 0x80, a nested stock
 * critical operation can leave BASEPRI = 0x80 inside the body. The CM7 workaround
 * can also clear an incoming PRIMASK. Kernel-aware exclusion still holds, and
 * unlock restores the captured input, but cannot undo intervening execution.
 * Keep incompatible native operations outside regions requiring continuous
 * stronger-mask protection. This API does not promise global IRQ exclusion.
 *
 * Other backends retain their native execution-context and exclusion contracts;
 * the FreeRTOS admission checks do not describe their enforcement.
 *
 * @return Opaque cookie for the matching @ref ove_irq_unlock.
 * @note Requires CONFIG_OVE_ASYNC. Disabled-feature stubs provide no exclusion.
 */
ove_irq_key_t ove_irq_lock(void);

/**
 * @brief Restore the state captured by the matching @ref ove_irq_lock.
 *
 * Each nested release restores its own captured state. The same-context and
 * LIFO contract and the documented native-body limitation apply to all bindings.
 * This does not make blocking or otherwise ineligible native calls valid.
 *
 * @param key Complete, unchanged cookie from the matching acquisition.
 * @note Requires CONFIG_OVE_ASYNC.
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
