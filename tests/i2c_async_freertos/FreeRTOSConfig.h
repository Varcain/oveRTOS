/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#include <assert.h>
#include "stm32f7xx.h"
#define configUSE_PREEMPTION 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 1
#define configUSE_PICOLIBC_TLS 1
#define configCPU_CLOCK_HZ 25000000
#define configMINIMAL_STACK_SIZE 128
#define configTIMER_TASK_STACK_DEPTH 128
#define configPRIO_BITS 4
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY (15 << 4)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (5 << 4)
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 5
#define configMAX_TASK_NAME_LEN 16
#define configUSE_16_BIT_TICKS 0
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configTOTAL_HEAP_SIZE (1024 * 1024)
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY 2
#define configTIMER_QUEUE_LENGTH 4
#define configASSERT(value) assert(value)
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTimerGetTimerDaemonTaskHandle 1
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#endif
