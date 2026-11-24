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
 * @brief Critical sections and interrupt-context queries.
 *
 * @note ove_is_in_isr() is always available. ove_sync_context() requires
 *       @c CONFIG_OVE_SYNC, @c CONFIG_OVE_QUEUE or @c CONFIG_OVE_EVENTGROUP,
 *       and the critical sections require @c CONFIG_OVE_ASYNC. Without their
 *       feature they are inline stubs that return zero and give no exclusion.
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
 * @brief Opaque interrupt-state key returned by @ref ove_irq_lock.
 *
 * Pass the complete, unchanged key to the matching @ref ove_irq_unlock and do
 * not inspect it. A task key is not necessarily zero.
 */
typedef uint64_t ove_irq_key_t;

#ifdef CONFIG_OVE_ASYNC

/**
 * @brief Enter a critical section of the backend's native exclusion domain.
 *
 * Nestable: pair each call exactly once with @ref ove_irq_unlock, in LIFO order
 * on the same thread or handler activation. Keep the body short: do not block,
 * yield, await, start the scheduler, migrate context, or change interrupt
 * priorities, grouping or privilege while it is held.
 *
 * On FreeRTOS only single-core ARMv7-M ports, without MPU wrappers or with V1
 * wrappers, are supported; other ports fail to compile. The section masks the
 * kernel-aware interrupts (configMAX_SYSCALL_INTERRUPT_PRIORITY and less
 * urgent); more urgent interrupts keep running and must not share state
 * protected by it. Callers may be privileged tasks, MPU V1 restricted tasks
 * and external IRQ, SysTick or PendSV handlers at kernel-aware priority. Any
 * other caller, or an ineligible priority grouping (checked once, at the first
 * lock), stops without returning a key, even with assertions disabled.
 * Restricted tasks keep their privilege level but need a serviceable
 * priority-zero SVCall with PRIMASK and FAULTMASK clear. This cannot be
 * checked before the SVCall: one that cannot be taken faults, and a non-zero
 * SVCall priority stops at the check. configPRIO_BITS must match the
 * hardware. Entry and exit may briefly mask faults; no latency bound is
 * promised.
 *
 * On FreeRTOS, lock and unlock never weaken an incoming PRIMASK, FAULTMASK or
 * stronger BASEPRI, and unlock restores the captured masks exactly. Native
 * calls inside the body can still lower a stronger incoming BASEPRI or clear
 * PRIMASK meanwhile, so this is not a global interrupt mask. Other backends
 * follow their native critical-section contracts.
 *
 * @return Key for the matching @ref ove_irq_unlock.
 * @note Requires CONFIG_OVE_ASYNC; the disabled stub gives no exclusion.
 */
ove_irq_key_t ove_irq_lock(void);

/**
 * @brief Leave the critical section entered by the matching @ref ove_irq_lock.
 *
 * Restores the state that call captured; each nested release restores its own.
 *
 * @param key Complete, unchanged key from the matching acquisition.
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
 */
bool ove_is_in_isr(void);

/** Native resource initialization/destruction is available in this context. */
#define OVE_SYNC_CONTEXT_RESOURCE (1u << 0)
/** Native mutex acquisitions and condition-variable operations are available. */
#define OVE_SYNC_CONTEXT_BLOCKING (1u << 1)
/** Task semaphore/event posts and serialized notification setup are available. */
#define OVE_SYNC_CONTEXT_PRODUCER (1u << 2)

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_QUEUE) || defined(CONFIG_OVE_EVENTGROUP)
/**
 * @brief Report which synchronization operations the calling context allows.
 *
 * Returns a mask of the flags below. A clear flag means the operation must not
 * be attempted: bindings reject it or, for destruction, retain the resource.
 * - @c OVE_SYNC_CONTEXT_RESOURCE: native initialization and destruction.
 * - @c OVE_SYNC_CONTEXT_BLOCKING: mutex acquisition and condition variables.
 * - @c OVE_SYNC_CONTEXT_PRODUCER: task semaphore/event posts and serialized
 *   notification setup only; it does not serialize registration with gives.
 *
 * RESOURCE and BLOCKING need a running, non-idle thread with interrupts
 * enabled; cooperative and scheduler-locked threads qualify. PRODUCER also
 * holds with interrupts masked, including in nested critical sections.
 * FreeRTOS also grants RESOURCE and PRODUCER before the scheduler starts, and
 * only PRODUCER while the scheduler is suspended.
 *
 * Returns zero in an ISR, in unprivileged code and outside the supported
 * domains (privileged Cortex-M3/M4/M7, Zephyr native_sim, single-core NuttX
 * flat sim and the host test backend), so also on FreeRTOS multicore, NuttX SMP
 * and FreeRTOS unless both INCLUDE_xTaskGetSchedulerState and
 * INCLUDE_xTaskGetIdleTaskHandle are 1. Inside a critical section on the host
 * test backend, only PRODUCER is set.
 *
 * The flags do not cover object validity, ownership or quiescence, and are not
 * the test for releasing an already-owned mutex, which can stay legal with
 * interrupts masked. The query touches no primitive, allocates nothing,
 * changes no interrupt mask and needs no TIME, ASYNC or thread wrappers.
 *
 * @return Context flags, or zero as described above.
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
