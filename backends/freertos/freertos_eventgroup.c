/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/eventgroup.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include "ove_eventgroup_validate.h"
#include "FreeRTOS.h"
#include "ove_ns_to_ticks.h"
#include "event_groups.h"
#include "task.h"
#include "timers.h"

#if configNUMBER_OF_CORES != 1
#error "oveRTOS deferred event groups require single-core FreeRTOS"
#endif

#if defined(portUSING_MPU_WRAPPERS) && portUSING_MPU_WRAPPERS == 1
#if configUSE_MPU_WRAPPERS_V1 != 1
#error "oveRTOS deferred event groups require MPU wrappers V1"
#endif
#define OVE_EVENTGROUP_MPU_V1 1
/* Keep the stock V1 privilege transition in its system-call entry section. */
void ove_eventgroup_deinit(ove_eventgroup_t eg) FREERTOS_SYSTEM_CALL __attribute__((noinline));
#endif

enum command_op { COMMAND_FREE, COMMAND_SET, COMMAND_CLEAR };
struct eventgroup_command {
	ove_eventgroup_t owner;
	ove_eventbits_t bits;
	enum command_op op;
};

_Static_assert(configTIMER_QUEUE_LENGTH > 0, "event groups require a positive timer queue length");
_Static_assert((uintmax_t)configTIMER_QUEUE_LENGTH <= (uintmax_t)(UBaseType_t) ~(UBaseType_t)0,
	       "event group timer queue length exceeds the native queue length type");
_Static_assert((uintmax_t)configTIMER_QUEUE_LENGTH < SIZE_MAX / sizeof(struct eventgroup_command),
	       "event group command pool size is not representable");
#define OVE_EVENTGROUP_COMMAND_COUNT ((size_t)configTIMER_QUEUE_LENGTH + 1u)

/* One slot per queued command, plus the command removed by the sole daemon.
 * Canceled slots stay occupied until their own queue message is consumed. */
static struct eventgroup_command commands[OVE_EVENTGROUP_COMMAND_COUNT] PRIVILEGED_DATA;

static void execute_command(void *data, uint32_t ignored)
{
	(void)ignored;
	struct eventgroup_command *command = data;
	ove_notify_cb notify = NULL;
	void *user_data = NULL;

	/* Exclude task teardown before selecting an owner. Native set may walk
	 * waiters, so interrupts must remain enabled outside metadata windows. */
	vTaskSuspendAll();
	taskENTER_CRITICAL();
	ove_eventgroup_t owner = command->owner;
	ove_eventbits_t bits = command->bits;
	enum command_op op = command->op;
	taskEXIT_CRITICAL();
	if (owner != NULL) {
		if (op == COMMAND_SET) {
			(void)xEventGroupSetBits(owner->handle, (EventBits_t)bits);
			if (bits != 0) {
				notify = owner->notify_cb;
				user_data = owner->notify_ud;
			}
		} else if (op == COMMAND_CLEAR) {
			(void)xEventGroupClearBits(owner->handle, (EventBits_t)bits);
		}
	}
	taskENTER_CRITICAL();
	command->owner = NULL;
	command->op = COMMAND_FREE;
	taskEXIT_CRITICAL();
	/* Both the owner and slot may expire or be reused during resume/notify.
	 * The remainder uses only detached locals. */
	(void)xTaskResumeAll();
	if (notify != NULL)
		notify(user_data);
}

static BaseType_t pend_command(ove_eventgroup_t owner, ove_eventbits_t bits, enum command_op op)
{
	BaseType_t result = pdFALSE;
	BaseType_t wake = pdFALSE;
#if configUSE_TRACE_FACILITY == 1
	if (op == COMMAND_SET) {
		traceENTER_xEventGroupSetBitsFromISR(owner->handle, (EventBits_t)bits, &wake);
		traceEVENT_GROUP_SET_BITS_FROM_ISR(owner->handle, (EventBits_t)bits);
	} else {
		traceENTER_xEventGroupClearBitsFromISR(owner->handle, (EventBits_t)bits);
		traceEVENT_GROUP_CLEAR_BITS_FROM_ISR(owner->handle, (EventBits_t)bits);
	}
#endif
	UBaseType_t key = taskENTER_CRITICAL_FROM_ISR();

	for (size_t i = 0; i < OVE_EVENTGROUP_COMMAND_COUNT; ++i) {
		struct eventgroup_command *command = &commands[i];
		if (command->op != COMMAND_FREE)
			continue;
		command->owner = owner;
		command->bits = bits;
		command->op = op;
		result = xTimerPendFunctionCallFromISR(execute_command, command, 0, &wake);
		if (result != pdPASS) {
			command->owner = NULL;
			command->op = COMMAND_FREE;
		}
		break;
	}
	taskEXIT_CRITICAL_FROM_ISR(key);
#if configUSE_TRACE_FACILITY == 1
	if (op == COMMAND_SET) {
		traceRETURN_xEventGroupSetBitsFromISR(result);
	} else {
		traceRETURN_xEventGroupClearBitsFromISR(result);
	}
#endif
	portYIELD_FROM_ISR(wake);
	return result;
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

ove_eventbits_t ove_eventgroup_valid_mask(void)
{
	return (ove_eventbits_t)(EventBits_t)~eventEVENT_BITS_CONTROL_BYTES;
}

int ove_eventgroup_init(ove_eventgroup_t *eg, ove_eventgroup_storage_t *storage)
{
	if (eg == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	EventGroupHandle_t native = xEventGroupCreateStatic(&storage->static_eg);
	if (native == NULL)
		return OVE_ERR_NO_MEMORY;
	storage->handle = native;
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	*eg = storage;
	return OVE_OK;
}

void ove_eventgroup_deinit(ove_eventgroup_t eg)
{
	if (eg == NULL)
		return;
#ifdef OVE_EVENTGROUP_MPU_V1
	BaseType_t was_privileged = portIS_PRIVILEGED();
	if (was_privileged == pdFALSE) {
		portRAISE_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#endif
	taskENTER_CRITICAL();
	for (size_t i = 0; i < OVE_EVENTGROUP_COMMAND_COUNT; ++i)
		if (commands[i].owner == eg)
			commands[i].owner = NULL;
	taskEXIT_CRITICAL();
	vEventGroupDelete(eg->handle);
#ifdef OVE_EVENTGROUP_MPU_V1
	if (was_privileged == pdFALSE) {
		portMEMORY_BARRIER();
		portRESET_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#endif
}

/* ─── Operations ─────────────────────────────────────────────────────── */

ove_eventbits_t ove_eventgroup_set_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	if (!ove_eventgroup_mask_valid(bits))
		return 0;

	ove_eventbits_t result = (ove_eventbits_t)xEventGroupSetBits(eg->handle, (EventBits_t)bits);
	if (bits != 0 && eg->notify_cb != NULL) {
		eg->notify_cb(eg->notify_ud);
	}
	return result;
}

ove_eventbits_t ove_eventgroup_clear_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	if (!ove_eventgroup_mask_valid(bits))
		return 0;

	return (ove_eventbits_t)xEventGroupClearBits(eg->handle, (EventBits_t)bits);
}

int ove_eventgroup_wait_bits(ove_eventgroup_t eg, ove_eventbits_t bits, uint32_t flags,
			     uint64_t timeout_ns, ove_eventbits_t *result)
{
	if (!ove_eventgroup_wait_input_valid(bits, flags))
		return OVE_ERR_INVALID_PARAM;

	BaseType_t wait_all = (flags & OVE_EG_WAIT_ALL) ? pdTRUE : pdFALSE;
	BaseType_t clear = (flags & OVE_EG_CLEAR_ON_EXIT) ? pdTRUE : pdFALSE;
	TickType_t ticks;
	EventBits_t val;

	if (timeout_ns == OVE_WAIT_FOREVER) {
		ticks = portMAX_DELAY;
	} else {
		ticks = ove_ns_to_ticks(timeout_ns);
	}

	val = xEventGroupWaitBits(eg->handle, (EventBits_t)bits, clear, wait_all, ticks);

	if (result != NULL) {
		*result = (ove_eventbits_t)val;
	}

	if (wait_all) {
		if ((val & bits) == bits) {
			return OVE_OK;
		}
	} else {
		if (val & bits) {
			return OVE_OK;
		}
	}
	return OVE_ERR_TIMEOUT;
}

ove_eventbits_t ove_eventgroup_set_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	if (!ove_eventgroup_mask_valid(bits))
		return 0;

	return pend_command(eg, bits, COMMAND_SET) == pdPASS ? bits : 0;
}

int ove_eventgroup_clear_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	if (!ove_eventgroup_mask_valid(bits))
		return OVE_ERR_INVALID_PARAM;

	/* FreeRTOS defers the clear to the timer service task. */
	return pend_command(eg, bits, COMMAND_CLEAR) == pdPASS ? OVE_OK : OVE_ERR_QUEUE_FULL;
}

ove_eventbits_t ove_eventgroup_get_bits(ove_eventgroup_t eg)
{
	return (ove_eventbits_t)xEventGroupGetBits(eg->handle);
}

int ove_eventgroup_set_notify(ove_eventgroup_t eg, ove_notify_cb cb, void *user_data)
{
	if (eg == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	eg->notify_cb = cb;
	eg->notify_ud = user_data;
	return OVE_OK;
}
