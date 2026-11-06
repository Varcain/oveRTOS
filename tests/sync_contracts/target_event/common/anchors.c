/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#if !defined(CONFIG_OVE_RTOS_FREERTOS) || !defined(CONFIG_OVE_SYNC) ||                        \
	defined(CONFIG_OVE_LINUX) || defined(CONFIG_OVE_ASYNC) || defined(CONFIG_OVE_TIME) || \
	defined(CONFIG_OVE_CONSOLE) || defined(CONFIG_OVE_INFER)
#error "this control requires the minimal FreeRTOS SYNC profile"
#endif

/* Explicit linker roots retain these values and each language's typed task
 * and ISR function pointers. None of the consumers runs at application entry. */
__attribute__((used)) const unsigned event_native_domain_values[] = {
	sizeof(ove_event_storage_t), _Alignof(ove_event_storage_t),
	sizeof(UBaseType_t),	     portUSING_MPU_WRAPPERS,
	configUSE_MPU_WRAPPERS_V1,   configMAX_SYSCALL_INTERRUPT_PRIORITY,
	configTICK_RATE_HZ,
};
