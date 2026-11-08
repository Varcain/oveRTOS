/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "FreeRTOS.h"
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
void vTaskSetTimeOutState(TimeOut_t *);
BaseType_t xTaskCheckForTimeOut(TimeOut_t *, TickType_t *);
void vTaskDelay(TickType_t);
BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *);
TaskHandle_t xTaskCreateStatic(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t,
			       StackType_t *, StaticTask_t *);
