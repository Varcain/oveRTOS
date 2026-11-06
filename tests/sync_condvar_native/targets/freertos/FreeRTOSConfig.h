/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
void condvar_target_reject(void);
#define configASSERT(x)                          \
	do {                                     \
		if (!(x))                        \
			condvar_target_reject(); \
	} while (0)
#define configUSE_PREEMPTION 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define __FPU_PRESENT 1
#define __FPU_USED 1
#define configUSE_PICOLIBC_TLS 1
#define configCPU_CLOCK_HZ 25000000
#define configMINIMAL_STACK_SIZE 2048
#define configPRIO_BITS 4
#define configKERNEL_INTERRUPT_PRIORITY (15 << 4)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (1U << 7)
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 5
#define configMAX_TASK_NAME_LEN 16
#define configUSE_16_BIT_TICKS 0
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_TIMERS 0
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_xSemaphoreGetMutexHolder 1
#endif
