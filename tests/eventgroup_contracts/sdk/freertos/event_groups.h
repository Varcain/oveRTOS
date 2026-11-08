/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "FreeRTOS.h"
typedef TickType_t EventBits_t;
#if OVE_MODEL_TICK_BITS == 16
#define eventEVENT_BITS_CONTROL_BYTES UINT16_C(0xff00)
#elif OVE_MODEL_TICK_BITS == 64
#define eventEVENT_BITS_CONTROL_BYTES UINT64_C(0xff00000000000000)
#else
#define eventEVENT_BITS_CONTROL_BYTES UINT32_C(0xff000000)
#endif
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *);
void vEventGroupDelete(EventGroupHandle_t);
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, BaseType_t, BaseType_t,
				TickType_t);
BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t, EventBits_t, BaseType_t *);
BaseType_t xEventGroupClearBitsFromISR(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupGetBits(EventGroupHandle_t);
