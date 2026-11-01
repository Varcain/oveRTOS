/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_TEST_SPI_FREERTOS_H
#define OVE_TEST_SPI_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
/* Host-only opaque SDK dependencies. The production storage header remains
 * authoritative; these placeholders do not measure the target ABI. */
typedef struct {
	uintptr_t opaque;
} StaticSemaphore_t;
typedef struct {
	uintptr_t opaque;
} StaticTask_t;
typedef struct {
	uintptr_t opaque;
} StaticQueue_t;
typedef struct {
	uintptr_t opaque;
} StaticTimer_t;
typedef struct {
	uintptr_t opaque;
} StaticEventGroup_t;
typedef uintptr_t StackType_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *TimerHandle_t;
typedef void *EventGroupHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
typedef void (*PendedFunction_t)(void *, uint32_t);
#define configTICK_RATE_HZ 1000
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portYIELD_FROM_ISR(wake) ((void)(wake))
TimerHandle_t xTimerCreateStatic(const char *, TickType_t, BaseType_t, void *,
				 TimerCallbackFunction_t, StaticTimer_t *);
BaseType_t xTimerStart(TimerHandle_t, TickType_t);
BaseType_t xTimerPendFunctionCall(PendedFunction_t, void *, uint32_t, TickType_t);
BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t, void *, uint32_t, BaseType_t *);
#endif
