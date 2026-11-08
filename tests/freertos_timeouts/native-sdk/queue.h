/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "FreeRTOS.h"
QueueHandle_t xQueueCreateStatic(UBaseType_t, UBaseType_t, uint8_t *, StaticQueue_t *);
void vQueueDelete(QueueHandle_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueSendFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueReceiveFromISR(QueueHandle_t, void *, BaseType_t *);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);
UBaseType_t uxQueueMessagesWaitingFromISR(QueueHandle_t);
UBaseType_t uxQueueGetQueueLength(QueueHandle_t);
