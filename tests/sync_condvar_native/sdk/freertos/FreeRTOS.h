/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_CONDVAR_MODEL_FREERTOS_H
#define OVE_CONDVAR_MODEL_FREERTOS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Complete host model objects; their sizes are not target ABI measurements. */
struct model_task;
typedef struct model_task *TaskHandle_t;
typedef struct {
	TaskHandle_t owner;
	unsigned int depth;
	bool initialized;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
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
typedef uint32_t StackType_t;
typedef void *QueueHandle_t;
typedef void *TimerHandle_t;
typedef void *EventGroupHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef uint32_t UBaseType_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
typedef void (*PendedFunction_t)(void *, uint32_t);
#ifndef configNUMBER_OF_CORES
#define configNUMBER_OF_CORES 1
#endif
#define configTICK_RATE_HZ 1000
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portYIELD_FROM_ISR(wake) ((void)(wake))

void *pvPortMalloc(size_t);
void vPortFree(void *);
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t *);

void model_enter_critical(void);
void model_exit_critical(void);
UBaseType_t model_enter_critical_from_isr(void);
void model_exit_critical_from_isr(UBaseType_t);
#define taskENTER_CRITICAL() model_enter_critical()
#define taskEXIT_CRITICAL() model_exit_critical()
#define taskENTER_CRITICAL_FROM_ISR() model_enter_critical_from_isr()
#define taskEXIT_CRITICAL_FROM_ISR(state) model_exit_critical_from_isr(state)
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *);
SemaphoreHandle_t xSemaphoreCreateCountingStatic(unsigned int, unsigned int, StaticSemaphore_t *);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
BaseType_t xTaskNotifyGive(TaskHandle_t);
#endif
