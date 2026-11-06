/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_SYNC_CONTRACT_FREERTOS_H
#define OVE_SYNC_CONTRACT_FREERTOS_H
#include "../../../sync_condvar_native/sdk/freertos/FreeRTOS.h"
BaseType_t xPortIsInsideInterrupt(void);
UBaseType_t model_enter_critical_from_isr(void);
void model_exit_critical_from_isr(UBaseType_t);
#define taskENTER_CRITICAL_FROM_ISR() model_enter_critical_from_isr()
#define taskEXIT_CRITICAL_FROM_ISR(state) model_exit_critical_from_isr(state)
#endif
