/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../../sync_condvar_native/sdk/freertos/FreeRTOS.h"
#ifndef OVE_MODEL_TIMEOUT_DEFINED
#define OVE_MODEL_TIMEOUT_DEFINED
typedef struct {
	BaseType_t xOverflowCount;
	TickType_t xTimeOnEntering;
} TimeOut_t;
void vTaskSetTimeOutState(TimeOut_t *);
BaseType_t xTaskCheckForTimeOut(TimeOut_t *, TickType_t *);
#endif
#define INCLUDE_xTaskAbortDelay OVE_MODEL_ABORT
#define INCLUDE_vTaskSuspend OVE_MODEL_SUSPEND
