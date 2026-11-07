/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "model.h"
#ifndef configNUMBER_OF_CORES
#define configNUMBER_OF_CORES 1
#endif
#ifndef INCLUDE_xTaskGetSchedulerState
#define INCLUDE_xTaskGetSchedulerState 0
#endif
#ifndef INCLUDE_xTaskGetIdleTaskHandle
#define INCLUDE_xTaskGetIdleTaskHandle 0
#endif
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
#define taskSCHEDULER_NOT_STARTED 1
#define taskSCHEDULER_RUNNING 2
#define taskSCHEDULER_SUSPENDED 0
int xPortIsInsideInterrupt(void);
#define taskENTER_CRITICAL() ((void)model_effect())
#define taskEXIT_CRITICAL() ((void)model_effect())
#define taskENTER_CRITICAL_FROM_ISR() model_effect()
#define taskEXIT_CRITICAL_FROM_ISR(key) ((void)(key), (void)model_effect())
