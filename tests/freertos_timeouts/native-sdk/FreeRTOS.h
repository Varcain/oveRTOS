/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../../sync_contracts/sdk/freertos/FreeRTOS.h"
#undef configTICK_RATE_HZ
#define configTICK_RATE_HZ ((uint64_t)OVE_TICK_RATE)
#define configMAX_PRIORITIES 32
#define tskIDLE_PRIORITY 0u
#define portUSING_MPU_WRAPPERS 0
#define configUSE_TRACE_FACILITY 0
#define INCLUDE_vTaskSuspend 1
#define configASSERT(condition) assert(condition)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000u))
typedef void (*TaskFunction_t)(void *);
typedef struct {
	TickType_t start;
	uint64_t wraps;
} TimeOut_t;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *);

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t *);
