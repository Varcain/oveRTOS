/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * FreeRTOS config for Renode-hosted STM32F746 test firmware, zero-heap
 * mode.  Mirrors tests/sim/freertos-qemu-zeroheap/FreeRTOSConfig.h with
 * the CPU clock retuned for the STM32F746 (216 MHz nominal) and the
 * interrupt-priority bits set for the STM32 NVIC (4 bits).
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* __FPU_PRESENT / __FPU_USED come from the CMSIS headers (stm32f746xx.h,
 * core_cm7.h) — do not redefine here; it would fight -Wredefined-macros
 * because CMSIS uses `1U` and we'd use `1`. */

/* Scheduler — nominal STM32F746 clock.  Renode approximates real time
 * at ~1 simulated second per wall-clock second under default settings, so
 * tick math works out without extra fudge factors. */
#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_DAEMON_TASK_STARTUP_HOOK       0
#define configCPU_CLOCK_HZ                       ((unsigned long)216000000)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMINIMAL_STACK_SIZE                 ((configSTACK_DEPTH_TYPE)256)
#define configMAX_TASK_NAME_LEN                  16
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    3

/* Memory — static allocation only, no heap */
#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         0

/* Synchronization */
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configQUEUE_REGISTRY_SIZE                10
#define configUSE_QUEUE_SETS                      0

/* Timers */
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                 (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                  10
#define configTIMER_TASK_STACK_DEPTH              (configMINIMAL_STACK_SIZE * 2)

/* Tasks */
#define configMAX_PRIORITIES                     7
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  3
#define configUSE_APPLICATION_TASK_TAG            1

/* Event groups and stream buffers */
#define configUSE_EVENT_GROUPS                   1
#define configUSE_STREAM_BUFFERS                 1

/* Co-routines (unused) */
#define configUSE_CO_ROUTINES                    0

/* Hook functions */
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             0

/* Run-time stats (disabled for test) */
#define configGENERATE_RUN_TIME_STATS            0
#define configUSE_TRACE_FACILITY                 0
#define configUSE_STATS_FORMATTING_FUNCTIONS     0

/* Include API functions */
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_xResumeFromISR                   0
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_eTaskGetState                    1
#define INCLUDE_xTimerPendFunctionCall           1

/* STM32F7 NVIC — 4 priority bits */
#define configPRIO_BITS                          4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY  15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY          (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Map FreeRTOS port handlers to CMSIS names */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler

/* Assert — use semihosting fprintf */
#include <stdio.h>
#include <stdlib.h>
#define configASSERT(x) do { if (!(x)) { fprintf(stderr, \
	"FreeRTOS assert failed: %s:%d\n", __FILE__, __LINE__); \
	exit(1); } } while (0)

#endif /* FREERTOS_CONFIG_H */
