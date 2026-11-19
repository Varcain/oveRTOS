/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../../sync_condvar_native/sdk/freertos/FreeRTOS.h"
#define configPRIO_BITS 4
#define __NVIC_PRIO_BITS 4
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 128
BaseType_t xPortIsInsideInterrupt(void);
#if OVE_MODEL_MPU
#define portUSING_MPU_WRAPPERS 1
#define configUSE_MPU_WRAPPERS_V1 1
#define FREERTOS_SYSTEM_CALL __attribute__((section("freertos_system_calls")))
BaseType_t model_privileged(void);
void model_raise(void);
void model_lower(void);
#define portIS_PRIVILEGED() model_privileged()
#define portRAISE_PRIVILEGE() model_raise()
#define portRESET_PRIVILEGE() model_lower()
#define portMEMORY_BARRIER() ((void)0)
#else
#define FREERTOS_SYSTEM_CALL
#endif
