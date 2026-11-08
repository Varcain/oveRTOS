/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <FreeRTOS.h>
BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t, void *, uint32_t, BaseType_t *);
