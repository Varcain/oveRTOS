/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
void eventgroup_target_reject(void);
#define configASSERT(value)                         \
	do {                                        \
		if (!(value))                       \
			eventgroup_target_reject(); \
	} while (0)
#define configUSE_PREEMPTION 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 1
#define __FPU_PRESENT 1
#define __FPU_USED 1
#define configUSE_PICOLIBC_TLS 1
#define configCPU_CLOCK_HZ 25000000
#define configMINIMAL_STACK_SIZE 1024
#define configPRIO_BITS 4
#define configKERNEL_INTERRUPT_PRIORITY (15 << 4)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (1U << 7)
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 6
#define configMAX_TASK_NAME_LEN 16
#define configUSE_16_BIT_TICKS 0
#define configSUPPORT_STATIC_ALLOCATION 1
#ifdef CONFIG_OVE_ZERO_HEAP
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#else
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#endif
#define configTOTAL_HEAP_SIZE (64 * 1024)
#ifdef OVE_MPU_V1
#define configKERNEL_PROVIDED_STATIC_MEMORY 0
#else
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#endif
#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY 3
#define configTIMER_QUEUE_LENGTH 3
#define configTIMER_TASK_STACK_DEPTH 1024
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_xTimerGetTimerDaemonTaskHandle 1
#ifdef OVE_MPU_V1
#define configUSE_MPU_WRAPPERS_V1 1
#define configTOTAL_MPU_REGIONS OVE_MPU_REGIONS
#define configENFORCE_SYSTEM_CALLS_FROM_KERNEL_ONLY 0
#define configALLOW_UNPRIVILEGED_CRITICAL_SECTIONS 0
#define configENABLE_ERRATA_837070_WORKAROUND 0
#endif
