/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../../sync_condvar_native/sdk/freertos/FreeRTOS.h"
#ifndef configTIMER_QUEUE_LENGTH
#define configTIMER_QUEUE_LENGTH OVE_MODEL_TIMER_DEPTH
#endif
#define portUSING_MPU_WRAPPERS OVE_MODEL_MPU
#ifndef configUSE_MPU_WRAPPERS_V1
#define configUSE_MPU_WRAPPERS_V1 1
#endif
#define FREERTOS_SYSTEM_CALL
#define PRIVILEGED_DATA
#define portMEMORY_BARRIER() __asm volatile("" ::: "memory")
BaseType_t model_privileged(void);
void model_raise(void);
void model_reset_privilege(void);
#define portIS_PRIVILEGED() model_privileged()
#define portRAISE_PRIVILEGE() model_raise()
#define portRESET_PRIVILEGE() model_reset_privilege()
void model_trace_enter(void *, uint32_t, bool, BaseType_t *);
void model_trace_event(void *, uint32_t, bool);
void model_trace_return(BaseType_t, bool);
#define traceENTER_xEventGroupSetBitsFromISR(owner, bits, wake) \
	model_trace_enter(owner, bits, false, wake)
#define traceEVENT_GROUP_SET_BITS_FROM_ISR(owner, bits) model_trace_event(owner, bits, false)
#define traceRETURN_xEventGroupSetBitsFromISR(result) model_trace_return(result, false)
#define traceENTER_xEventGroupClearBitsFromISR(owner, bits) \
	model_trace_enter(owner, bits, true, NULL)
#define traceEVENT_GROUP_CLEAR_BITS_FROM_ISR(owner, bits) model_trace_event(owner, bits, true)
#define traceRETURN_xEventGroupClearBitsFromISR(result) model_trace_return(result, true)
