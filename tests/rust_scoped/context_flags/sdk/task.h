/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "FreeRTOS.h"
#if INCLUDE_xTaskGetSchedulerState == 1
BaseType_t xTaskGetSchedulerState(void);
#endif
TaskHandle_t xTaskGetCurrentTaskHandle(void);
#if configNUMBER_OF_CORES == 1 && INCLUDE_xTaskGetIdleTaskHandle == 1
TaskHandle_t xTaskGetIdleTaskHandle(void);
#endif
