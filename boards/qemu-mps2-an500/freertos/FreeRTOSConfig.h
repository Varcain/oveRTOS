/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "ove_config.h"

/* Cortex-M7 FPU (MPS2-AN500 has FPv5-SP) */
#define __FPU_PRESENT 1
#define __FPU_USED 1

/* Picolibc TLS integration: per-task errno (and any other __thread
 * storage).  Without this errno is a single global shared across all
 * tasks, a real bug magnet for any code that checks errno after a
 * yield-prone call.  Setting configUSE_PICOLIBC_TLS=1 makes
 * FreeRTOS.h include picolibc-freertos.h (from the FreeRTOS kernel
 * include dir), which provides configINIT_TLS_BLOCK / configSET_TLS_BLOCK
 * at task switch and bumps minimum task stack by _tls_size() bytes
 * (typically ~32 to 64 B for errno-only TLS). */
#define configUSE_PICOLIBC_TLS 1

/* Scheduler */
#define configUSE_PREEMPTION 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 1
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0
#define configCPU_CLOCK_HZ ((unsigned long)25000000)
#define configTICK_RATE_HZ ((TickType_t)1000)
#define configMINIMAL_STACK_SIZE ((configSTACK_DEPTH_TYPE)256)
#define configMAX_TASK_NAME_LEN 16
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 3

/* Memory */
#define configSUPPORT_STATIC_ALLOCATION 1
#ifdef CONFIG_OVE_ZERO_HEAP
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#else
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#endif
#define configTOTAL_HEAP_SIZE ((size_t)(256 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP 0

/* Synchronization */
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configQUEUE_REGISTRY_SIZE 10
#define configUSE_QUEUE_SETS 0

/* Timers */
#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH 10
#define configTIMER_TASK_STACK_DEPTH (configMINIMAL_STACK_SIZE * 2)

/* Tasks */
#define configMAX_PRIORITIES 7
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 3
#define configUSE_APPLICATION_TASK_TAG 1

/* Event groups and stream buffers */
#define configUSE_EVENT_GROUPS 1
#define configUSE_STREAM_BUFFERS 1

/* Co-routines (unused) */
#define configUSE_CO_ROUTINES 0

/* Hook functions */
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 0

/* Run-time stats — uses a SysTick-based ms counter since QEMU
 * doesn't emulate the DWT cycle counter. */
#define configGENERATE_RUN_TIME_STATS 1
#define configUSE_TRACE_FACILITY 1
#define configRECORD_STACK_HIGH_ADDRESS 1
#if configSUPPORT_DYNAMIC_ALLOCATION
#define configUSE_STATS_FORMATTING_FUNCTIONS 1
#else
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#endif

/* Include API functions */
#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xResumeFromISR 0
#define INCLUDE_vTaskDelayUntil 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_xTimerPendFunctionCall 1

/* Cortex-M7 interrupt priority configuration (MPS2-AN500) */
#define configPRIO_BITS 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
	(configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
	(configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Run-time stats timer — ms counter incremented from vApplicationTickHook() */
extern volatile uint32_t ove_runtime_counter_ms;
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() (ove_runtime_counter_ms = 0UL)
#define portGET_RUN_TIME_COUNTER_VALUE() ove_runtime_counter_ms

/* Trace hooks wired to oveRTOS trace backend (backends/freertos/freertos_trace.c).
 * SWITCHED_IN/OUT fire from inside vTaskSwitchContext() (PendSV context); the
 * blocking macros fire in task context before the yield. See the header comment
 * in freertos_trace.c for the reasoning behind the split. */
#ifdef CONFIG_OVE_TRACE_STREAM
extern void ove_backend_trace_task_switched_in(void);
extern void ove_backend_trace_task_switched_out(void);
extern void ove_backend_trace_task_blocking(void);
#define traceTASK_SWITCHED_IN() ove_backend_trace_task_switched_in()
#define traceTASK_SWITCHED_OUT() ove_backend_trace_task_switched_out()
#define traceBLOCKING_ON_QUEUE_RECEIVE(pxQueue) ove_backend_trace_task_blocking()
#define traceBLOCKING_ON_QUEUE_SEND(pxQueue) ove_backend_trace_task_blocking()
#define traceBLOCKING_ON_QUEUE_PEEK(pxQueue) ove_backend_trace_task_blocking()
#define traceTASK_DELAY() ove_backend_trace_task_blocking()
#define traceTASK_DELAY_UNTIL(xTimeToWake) ove_backend_trace_task_blocking()
#endif

/* Map FreeRTOS port handlers to CMSIS names. The Linux personality seam owns the
 * SVC_Handler vector (to trap a loaded program's svc), so under CONFIG_OVE_LINUX
 * this alias is dropped: FreeRTOS's handler stays named vPortSVCHandler and the
 * seam (backends/freertos/freertos_lnx.c) forwards the start-scheduler svc to it. */
#ifndef CONFIG_OVE_LINUX
#define vPortSVCHandler SVC_Handler
#endif
#define xPortPendSVHandler PendSV_Handler

#ifdef CONFIG_OVE_LINUX
/* Linux guest MPU isolation (ARM_CM4_MPU port): each program runs UNPRIVILEGED in a
 * per-task MPU region set so a stray access faults instead of corrupting the kernel.
 * Values verified against QEMU's an500 Cortex-M7 via the gdbstub: MPU_TYPE.DREGION = 8,
 * CPUID = 0x411fc272 (r1p2 — so the r0p0/r0p1 errata workaround must stay OFF, else
 * prvSetupMPU's configASSERT hangs at boot). Our SVC vector is the seam's strong
 * SVC_Handler (not vPortSVCHandler) → configCHECK_HANDLER_INSTALLATION must be 0. */
#define configUSE_MPU_WRAPPERS_V1                       1
/* 16, NOT 8: QEMU's mps2-an500 Cortex-M7 reports MPU_TYPE.DREGION=16, and the ARM_CM4_MPU port
 * SILENTLY SKIPS all MPU setup — the region programming AND the CTRL ENABLE|BACKGROUND(=PRIVDEFENA)
 * write — when configTOTAL_MPU_REGIONS != the hardware region count (port.c prvSetupMPU:
 * `if (portMPU_TYPE_REG == (configTOTAL_MPU_REGIONS << 8))`). A mismatch (the old 8) HardFaults at
 * scheduler start: prvRestoreContextOfFirstTask enables the MPU with no regions + PRIVDEFENA off, so
 * the first privileged instruction fetch is a MemManage IACCVIOL. The real STM32F746 M7 has 8 MPU
 * regions, so its own board config keeps 8. */
#define configTOTAL_MPU_REGIONS                         16
#define configENABLE_ERRATA_837070_WORKAROUND           0
#define configCHECK_HANDLER_INSTALLATION                0
#define configENFORCE_SYSTEM_CALLS_FROM_KERNEL_ONLY     0
#define configALLOW_UNPRIVILEGED_CRITICAL_SECTIONS      0
#endif /* CONFIG_OVE_LINUX */

#endif /* FREERTOS_CONFIG_H */
