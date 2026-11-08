/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "FreeRTOS.h"
typedef uint32_t EventBits_t;
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *);
void vEventGroupDelete(EventGroupHandle_t);
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, BaseType_t, BaseType_t,
				TickType_t);
BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t, EventBits_t, BaseType_t *);
BaseType_t xEventGroupClearBitsFromISR(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupGetBits(EventGroupHandle_t);
