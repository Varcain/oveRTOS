/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * FreeRTOS backend for ove/irq.h.
 *
 * Retain native task nesting or the native ISR cookie and save architectural
 * masks separately. Checked entry protects safe bindings before returning a
 * key. The body uses the native kernel-aware domain; native operations inside
 * it can still lower a stronger caller-owned raw mask.
 *
 * Context detection is available with SYNC, ASYNC, QUEUE or EVENTGROUP; critical sections
 * require ASYNC.
 */

#include "ove/irq.h"
#include "ove/types.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_ASYNC) || defined(CONFIG_OVE_QUEUE) || \
	defined(CONFIG_OVE_EVENTGROUP)

#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include <stdint.h>
#include "ove_sync_context.h"

#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_QUEUE) || defined(CONFIG_OVE_EVENTGROUP)
uint32_t ove_sync_context(void)
{
#if defined(OVE_SYNC_CORTEX_M) && (configNUMBER_OF_CORES == 1) && \
	(INCLUDE_xTaskGetSchedulerState == 1) && (INCLUDE_xTaskGetIdleTaskHandle == 1)
	if (!ove_sync_privileged() || xPortIsInsideInterrupt())
		return 0;
	BaseType_t state = xTaskGetSchedulerState();
	if (state == taskSCHEDULER_NOT_STARTED)
		return OVE_SYNC_CONTEXT_RESOURCE | OVE_SYNC_CONTEXT_PRODUCER;
	if (state != taskSCHEDULER_RUNNING && state != taskSCHEDULER_SUSPENDED)
		return 0;
	bool blocking = state == taskSCHEDULER_RUNNING && ove_sync_irqs_enabled();
	if (xTaskGetCurrentTaskHandle() == xTaskGetIdleTaskHandle())
		return 0;
	return OVE_SYNC_CONTEXT_PRODUCER |
	       (blocking ? OVE_SYNC_CONTEXT_RESOURCE | OVE_SYNC_CONTEXT_BLOCKING : 0u);
#else
	return 0;
#endif
}
#endif

#ifdef CONFIG_OVE_ASYNC
#include <ove_irq_mask_ops.h>
#define OVE_IRQ_KEY_FROM_ISR_BIT (UINT64_C(1) << 63)

#if defined(OVE_IRQ_MASK_CORTEX_M) && defined(OVE_FREERTOS_IRQ_PORT_SUPPORTED) &&    \
	OVE_FREERTOS_IRQ_PORT_SUPPORTED == 1 && (configNUMBER_OF_CORES == 1) &&      \
	defined(configPRIO_BITS) && defined(configMAX_SYSCALL_INTERRUPT_PRIORITY) && \
	(!defined(portUSING_MPU_WRAPPERS) || portUSING_MPU_WRAPPERS == 0 ||          \
	 (defined(configUSE_MPU_WRAPPERS_V1) && configUSE_MPU_WRAPPERS_V1 == 1))
#define OVE_IRQ_MASK_SUPPORTED 1
_Static_assert(sizeof(UBaseType_t) <= sizeof(uint32_t), "native IRQ cookie must fit completely");
struct irq_masks {
	uint32_t primask, basepri, faultmask;
};
static inline uint32_t irq_threshold(void)
{
	unsigned bits = configPRIO_BITS;
	uint64_t threshold = configMAX_SYSCALL_INTERRUPT_PRIORITY;
	if (bits < 1u || bits > 8u || threshold == 0 || threshold > 255u)
		irq_mask_reject();
#ifdef __NVIC_PRIO_BITS
	if (bits != __NVIC_PRIO_BITS)
		irq_mask_reject();
#endif
	unsigned shift = 8u - bits;
	if ((threshold & ((1u << shift) - 1u)) || (bits == 8u && (threshold & 1u)) ||
	    irq_mask_group() > (bits == 8u ? 0u : 7u - bits))
		irq_mask_reject();
	return (uint32_t)threshold;
}
static inline void irq_handler_check(uint32_t exception, uint32_t threshold)
{
	if ((exception < 16u && exception != 14u && exception != 15u) || exception >= 256u)
		irq_mask_reject();
	if (irq_mask_priority(exception) < threshold)
		irq_mask_reject();
}
static inline struct irq_masks irq_snapshot(void)
{
	return (struct irq_masks){event_mask_primask(), event_mask_basepri(),
				  event_mask_faultmask()};
}
static inline ove_irq_key_t irq_enter(bool isr, uint32_t threshold)
{
	struct irq_masks saved = irq_snapshot();
	if (saved.primask || (saved.basepri && saved.basepri < threshold))
		event_mask_disable_faults();
	UBaseType_t native = 0;
	if (isr)
		native = taskENTER_CRITICAL_FROM_ISR();
	else
		taskENTER_CRITICAL();
	/* The shared BASEPRI write also requires this CM7 envelope. */
	event_mask_disable_irqs();
	event_mask_restore_basepri(saved.basepri && saved.basepri < threshold ? saved.basepri
									      : threshold);
	event_mask_restore_primask(saved.primask);
	event_mask_restore_faultmask(saved.faultmask);
	return (uint64_t)native | ((uint64_t)saved.basepri << 32) |
	       ((uint64_t)saved.primask << 40) | ((uint64_t)saved.faultmask << 41) |
	       (isr ? OVE_IRQ_KEY_FROM_ISR_BIT : 0);
}
static inline void irq_leave(ove_irq_key_t key)
{
	struct irq_masks saved = {(uint32_t)((key >> 40) & 1u), (uint32_t)((key >> 32) & 255u),
				  (uint32_t)((key >> 41) & 1u)};
	struct irq_masks current = irq_snapshot();
	event_mask_disable_irqs();
	if (saved.faultmask && !current.faultmask)
		event_mask_disable_faults();
	if (key & OVE_IRQ_KEY_FROM_ISR_BIT)
		taskEXIT_CRITICAL_FROM_ISR((UBaseType_t)(uint32_t)key);
	else
		taskEXIT_CRITICAL();
	event_mask_restore_basepri(saved.basepri);
	event_mask_restore_primask(saved.primask);
	event_mask_restore_faultmask(saved.faultmask);
}
#if defined(portUSING_MPU_WRAPPERS) && portUSING_MPU_WRAPPERS == 1
#define OVE_IRQ_MPU_V1 1
static ove_irq_key_t irq_task_enter(void) FREERTOS_SYSTEM_CALL __attribute__((noinline));
static void irq_task_leave(ove_irq_key_t key) FREERTOS_SYSTEM_CALL __attribute__((noinline));
#endif
static inline __attribute__((always_inline)) bool irq_task_promote(void)
{
#ifdef OVE_IRQ_MPU_V1
	bool promoted = portIS_PRIVILEGED() == pdFALSE;
	if (promoted) {
		/* Emit the SVC in the permitted, noninline task helper. */
		portRAISE_PRIVILEGE();
		portMEMORY_BARRIER();
		if (portIS_PRIVILEGED() == pdFALSE)
			irq_mask_reject();
	}
	if (irq_mask_priority(11u) != 0)
		irq_mask_reject();
	return promoted;
#else
	if (irq_mask_control() & 1u)
		irq_mask_reject();
	return false;
#endif
}
static inline void irq_task_demote(bool promoted)
{
#ifdef OVE_IRQ_MPU_V1
	if (promoted) {
		portMEMORY_BARRIER();
		portRESET_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#else
	(void)promoted;
#endif
}
static ove_irq_key_t irq_task_enter(void)
{
	bool promoted = irq_task_promote();
	uint32_t threshold = irq_threshold();
	ove_irq_key_t key = irq_enter(false, threshold);
	irq_task_demote(promoted);
	return key;
}
static void irq_task_leave(ove_irq_key_t key)
{
	bool promoted = irq_task_promote();
	(void)irq_threshold();
	irq_leave(key);
	irq_task_demote(promoted);
}
#endif

ove_irq_key_t ove_irq_lock(void)
{
#ifdef OVE_IRQ_MASK_SUPPORTED
	uint32_t exception = irq_mask_exception();
	if (exception) {
		uint32_t threshold = irq_threshold();
		irq_handler_check(exception, threshold);
		return irq_enter(true, threshold);
	}
	return irq_task_enter();
#else
	irq_mask_reject();
#endif
}
void ove_irq_unlock(ove_irq_key_t key)
{
#ifdef OVE_IRQ_MASK_SUPPORTED
	uint32_t exception = irq_mask_exception();
	if ((exception != 0) != ((key & OVE_IRQ_KEY_FROM_ISR_BIT) != 0))
		irq_mask_reject();
	if (exception) {
		uint32_t threshold = irq_threshold();
		irq_handler_check(exception, threshold);
		irq_leave(key);
	} else {
		irq_task_leave(key);
	}
#else
	(void)key;
	irq_mask_reject();
#endif
}

#endif /* CONFIG_OVE_ASYNC */

bool ove_is_in_isr(void)
{
	return xPortIsInsideInterrupt() != 0;
}

#endif /* CONFIG_OVE_SYNC || CONFIG_OVE_ASYNC || CONFIG_OVE_QUEUE || CONFIG_OVE_EVENTGROUP */
