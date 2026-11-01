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
#endif
