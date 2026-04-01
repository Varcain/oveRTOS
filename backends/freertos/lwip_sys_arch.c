/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * lwIP sys_arch port for FreeRTOS.
 *
 * Provides the OS abstraction layer required by lwIP when NO_SYS=0.
 * Supports both heap-based and zero-heap (static pool) allocation.
 */

#include "ove_config.h"
#include "lwip/sys.h"
#include "lwip/opt.h"
#include "lwip/stats.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* ── Zero-heap static pools ─────────────────────────────────── */

#ifdef CONFIG_OVE_ZERO_HEAP

#ifndef CONFIG_OVE_NET_LWIP_SYS_SEM_POOL
#define CONFIG_OVE_NET_LWIP_SYS_SEM_POOL    16
#endif
#ifndef CONFIG_OVE_NET_LWIP_SYS_MUTEX_POOL
#define CONFIG_OVE_NET_LWIP_SYS_MUTEX_POOL   8
#endif
#ifndef CONFIG_OVE_NET_LWIP_SYS_MBOX_POOL
#define CONFIG_OVE_NET_LWIP_SYS_MBOX_POOL   16
#endif
#ifndef CONFIG_OVE_NET_LWIP_SYS_MBOX_DEPTH
#define CONFIG_OVE_NET_LWIP_SYS_MBOX_DEPTH  32
#endif
#ifndef CONFIG_OVE_NET_LWIP_SYS_THREAD_POOL
#define CONFIG_OVE_NET_LWIP_SYS_THREAD_POOL  4
#endif
#ifndef CONFIG_OVE_NET_LWIP_SYS_THREAD_STACK
#define CONFIG_OVE_NET_LWIP_SYS_THREAD_STACK 8192
#endif

/* Semaphore pool */
static struct {
	StaticSemaphore_t buf;
	SemaphoreHandle_t handle;
	int in_use;
} s_sem_pool[CONFIG_OVE_NET_LWIP_SYS_SEM_POOL];

/* Mutex pool */
static struct {
	StaticSemaphore_t buf;
	SemaphoreHandle_t handle;
	int in_use;
} s_mtx_pool[CONFIG_OVE_NET_LWIP_SYS_MUTEX_POOL];

/* Mailbox pool */
static struct {
	StaticQueue_t     buf;
	uint8_t           storage[CONFIG_OVE_NET_LWIP_SYS_MBOX_DEPTH * sizeof(void *)];
	QueueHandle_t     handle;
	int               in_use;
} s_mbox_pool[CONFIG_OVE_NET_LWIP_SYS_MBOX_POOL];

/* Thread pool */
#define THREAD_STACK_WORDS (CONFIG_OVE_NET_LWIP_SYS_THREAD_STACK / sizeof(StackType_t))
static struct {
	StaticTask_t tcb;
	StackType_t  stack[THREAD_STACK_WORDS];
	TaskHandle_t handle;
	int          in_use;
} s_thread_pool[CONFIG_OVE_NET_LWIP_SYS_THREAD_POOL];

#endif /* CONFIG_OVE_ZERO_HEAP */

/* ---------- Initialisation ---------- */

void sys_init(void)
{
}

/* ---------- Time ---------- */

u32_t sys_now(void)
{
	return (u32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---------- Semaphores ---------- */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	for (int i = 0; i < CONFIG_OVE_NET_LWIP_SYS_SEM_POOL; i++) {
		if (!s_sem_pool[i].in_use) {
			s_sem_pool[i].handle = xSemaphoreCreateCountingStatic(
				0xFFFF, count, &s_sem_pool[i].buf);
			s_sem_pool[i].in_use = 1;
			*sem = s_sem_pool[i].handle;
			return ERR_OK;
		}
	}
	*sem = NULL;
	return ERR_MEM;
#else
	*sem = xSemaphoreCreateCounting(0xFFFF, count);
	if (*sem == NULL) return ERR_MEM;
	return ERR_OK;
#endif
}

void sys_sem_free(sys_sem_t *sem)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	for (int i = 0; i < CONFIG_OVE_NET_LWIP_SYS_SEM_POOL; i++) {
		if (s_sem_pool[i].handle == *sem) {
			s_sem_pool[i].in_use = 0;
			s_sem_pool[i].handle = NULL;
			break;
		}
	}
#else
	if (*sem) vSemaphoreDelete(*sem);
#endif
}

void sys_sem_signal(sys_sem_t *sem)
{
	xSemaphoreGive(*sem);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
	TickType_t ticks = (timeout == 0) ? portMAX_DELAY
					  : pdMS_TO_TICKS(timeout);
	TickType_t start = xTaskGetTickCount();
	if (xSemaphoreTake(*sem, ticks) == pdTRUE) {
		TickType_t elapsed = (xTaskGetTickCount() - start) *
				     portTICK_PERIOD_MS;
		return (u32_t)elapsed;
	}
	return SYS_ARCH_TIMEOUT;
}

int sys_sem_valid(sys_sem_t *sem)
{
	return (*sem != NULL) ? 1 : 0;
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
	*sem = NULL;
}

/* ---------- Mutexes ---------- */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	for (int i = 0; i < CONFIG_OVE_NET_LWIP_SYS_MUTEX_POOL; i++) {
		if (!s_mtx_pool[i].in_use) {
			s_mtx_pool[i].handle = xSemaphoreCreateMutexStatic(
				&s_mtx_pool[i].buf);
			s_mtx_pool[i].in_use = 1;
			*mutex = s_mtx_pool[i].handle;
			return ERR_OK;
		}
	}
	*mutex = NULL;
	return ERR_MEM;
#else
	*mutex = xSemaphoreCreateMutex();
	if (*mutex == NULL) return ERR_MEM;
	return ERR_OK;
#endif
}

void sys_mutex_free(sys_mutex_t *mutex)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	for (int i = 0; i < CONFIG_OVE_NET_LWIP_SYS_MUTEX_POOL; i++) {
		if (s_mtx_pool[i].handle == *mutex) {
			s_mtx_pool[i].in_use = 0;
			s_mtx_pool[i].handle = NULL;
			break;
		}
	}
#else
	if (*mutex) vSemaphoreDelete(*mutex);
#endif
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
	xSemaphoreTake(*mutex, portMAX_DELAY);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
	xSemaphoreGive(*mutex);
}

int sys_mutex_valid(sys_mutex_t *mutex)
{
	return (*mutex != NULL) ? 1 : 0;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
	*mutex = NULL;
}

/* ---------- Mailboxes ---------- */

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	(void)size;
	for (int i = 0; i < CONFIG_OVE_NET_LWIP_SYS_MBOX_POOL; i++) {
		if (!s_mbox_pool[i].in_use) {
			s_mbox_pool[i].handle = xQueueCreateStatic(
				CONFIG_OVE_NET_LWIP_SYS_MBOX_DEPTH,
				sizeof(void *),
				s_mbox_pool[i].storage,
				&s_mbox_pool[i].buf);
			s_mbox_pool[i].in_use = 1;
			*mbox = s_mbox_pool[i].handle;
			return ERR_OK;
		}
	}
	*mbox = NULL;
	return ERR_MEM;
#else
	*mbox = xQueueCreate((UBaseType_t)size, sizeof(void *));
	if (*mbox == NULL) return ERR_MEM;
	return ERR_OK;
#endif
}

void sys_mbox_free(sys_mbox_t *mbox)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	for (int i = 0; i < CONFIG_OVE_NET_LWIP_SYS_MBOX_POOL; i++) {
		if (s_mbox_pool[i].handle == *mbox) {
			s_mbox_pool[i].in_use = 0;
			s_mbox_pool[i].handle = NULL;
			break;
		}
	}
#else
	if (*mbox) vQueueDelete(*mbox);
#endif
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
	xQueueSendToBack(*mbox, &msg, portMAX_DELAY);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
	if (xQueueSendToBack(*mbox, &msg, 0) == pdTRUE)
		return ERR_OK;
	return ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
	BaseType_t wake = pdFALSE;
	if (xQueueSendToBackFromISR(*mbox, &msg, &wake) == pdTRUE) {
		portYIELD_FROM_ISR(wake);
		return ERR_OK;
	}
	return ERR_MEM;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
	void *tmp = NULL;
	TickType_t ticks = (timeout == 0) ? portMAX_DELAY
					  : pdMS_TO_TICKS(timeout);
	TickType_t start = xTaskGetTickCount();
	if (xQueueReceive(*mbox, &tmp, ticks) == pdTRUE) {
		if (msg) *msg = tmp;
		TickType_t elapsed = (xTaskGetTickCount() - start) *
				     portTICK_PERIOD_MS;
		return (u32_t)elapsed;
	}
	return SYS_ARCH_TIMEOUT;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
	void *tmp = NULL;
	if (xQueueReceive(*mbox, &tmp, 0) == pdTRUE) {
		if (msg) *msg = tmp;
		return 0;
	}
	return SYS_MBOX_EMPTY;
}

int sys_mbox_valid(sys_mbox_t *mbox)
{
	return (*mbox != NULL) ? 1 : 0;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
	*mbox = NULL;
}

/* ---------- Threads ---------- */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
			    void *arg, int stacksize, int prio)
{
	TaskHandle_t task = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	(void)stacksize;
	for (int i = 0; i < CONFIG_OVE_NET_LWIP_SYS_THREAD_POOL; i++) {
		if (!s_thread_pool[i].in_use) {
			s_thread_pool[i].handle = xTaskCreateStatic(
				thread, name, THREAD_STACK_WORDS,
				arg, (UBaseType_t)prio,
				s_thread_pool[i].stack,
				&s_thread_pool[i].tcb);
			s_thread_pool[i].in_use = 1;
			task = s_thread_pool[i].handle;
			break;
		}
	}
#else
	xTaskCreate(thread, name, (configSTACK_DEPTH_TYPE)(stacksize / 4),
		    arg, (UBaseType_t)prio, &task);
#endif
	return task;
}

/* ---------- Critical sections ---------- */

sys_prot_t sys_arch_protect(void)
{
	taskENTER_CRITICAL();
	return 1;
}

void sys_arch_unprotect(sys_prot_t pval)
{
	(void)pval;
	taskEXIT_CRITICAL();
}

#ifdef CONFIG_OVE_ZERO_HEAP
static StaticSemaphore_t s_lwip_mutex_buf;
#endif
static SemaphoreHandle_t s_lwip_mutex;

void sys_lock_tcpip_core(void)
{
	if (!s_lwip_mutex) {
#ifdef CONFIG_OVE_ZERO_HEAP
		s_lwip_mutex = xSemaphoreCreateMutexStatic(&s_lwip_mutex_buf);
#else
		s_lwip_mutex = xSemaphoreCreateMutex();
#endif
	}
	xSemaphoreTake(s_lwip_mutex, portMAX_DELAY);
}

void sys_unlock_tcpip_core(void)
{
	xSemaphoreGive(s_lwip_mutex);
}
