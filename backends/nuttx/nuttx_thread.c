/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/thread.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include <nuttx/semaphore.h>
#include <nuttx/tls.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>

/* Per-task pointer via NuttX task TLS */
static int tls_index = -1;

static void tls_ensure_init(void)
{
	if (tls_index < 0) {
		tls_index = task_tls_alloc(NULL);
	}
}

static struct ove_thread *tls_get_current(void)
{
	if (tls_index < 0) {
		return NULL;
	}
	return (struct ove_thread *)(uintptr_t)task_tls_get_value(tls_index);
}

static void tls_set_current(struct ove_thread *t)
{
	tls_ensure_init();
	task_tls_set_value(tls_index, (uintptr_t)t);
}

/* Store first thread for join in start_scheduler */
static struct ove_thread *first_thread;


/* SIGUSR1 handler installed once */
static volatile int sig_handler_installed;

static int map_priority(ove_prio_t prio)
{
	/* NuttX SCHED_FIFO: higher number = higher priority */
	switch (prio) {
	case OVE_PRIO_IDLE:         return 50;
	case OVE_PRIO_LOW:          return 60;
	case OVE_PRIO_BELOW_NORMAL: return 80;
	case OVE_PRIO_NORMAL:       return 100;
	case OVE_PRIO_ABOVE_NORMAL: return 120;
	case OVE_PRIO_HIGH:         return 150;
	case OVE_PRIO_REALTIME:     return 200;
	case OVE_PRIO_CRITICAL:     return 220;
	default:                        return 100;
	}
}

static void sigusr1_handler(int sig)
{
	(void)sig;
	struct ove_thread *t = tls_get_current();
	if (t && t->suspend_inited) {
		t->state = OVE_THREAD_STATE_SUSPENDED;
		/* Block until resumed via nxsem_post */
		while (t->state == OVE_THREAD_STATE_SUSPENDED) {
			nxsem_wait(&t->suspend_sem);
		}
	}
}

static void ensure_sigusr1_handler(void)
{
	if (!sig_handler_installed) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = sigusr1_handler;
		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR1, &sa, NULL);
		sig_handler_installed = 1;
	}
}

static int task_wrapper(int argc, char *argv[])
{
	struct ove_thread *t;

	(void)argc;
	t = (struct ove_thread *)strtoul(argv[1], NULL, 0);
	tls_set_current(t);

	t->state = OVE_THREAD_STATE_RUNNING;
	t->entry(t->arg);
	t->state = OVE_THREAD_STATE_TERMINATED;
	nxsem_post(&t->done_sem);
	return 0;
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

static int thread_start(struct ove_thread *t,
			const struct ove_thread_desc *desc)
{
	char addr_str[20];
	int pid;
	size_t stack;

	t->entry = desc->entry;
	t->arg = desc->arg;
	t->state = OVE_THREAD_STATE_READY;
	t->suspend_inited = 0;
	nxsem_init(&t->done_sem, 0, 0);

	ensure_sigusr1_handler();

	stack = desc->stack_size;
	if (stack == 0) {
		stack = 2048;
	}

	snprintf(addr_str, sizeof(addr_str), "0x%lx",
		 (unsigned long)(uintptr_t)t);
	{
		char *argv_args[] = { addr_str, NULL };
		pid = task_create(desc->name ? desc->name : "ove_thread",
				  map_priority(desc->priority), (int)stack,
				  task_wrapper, argv_args);
	}
	if (pid < 0) {
		nxsem_destroy(&t->done_sem);
		return OVE_ERR_NO_MEMORY;
	}

	t->pid = pid;
	t->started = 1;

	if (first_thread == NULL) {
		first_thread = t;
	}

	return OVE_OK;
}

int ove_thread_init(ove_thread_t *handle,
			ove_thread_storage_t *storage,
			const struct ove_thread_desc *desc)
{
	if (handle == NULL || storage == NULL || desc == NULL ||
	    desc->entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	memset(storage, 0, sizeof(*storage));
	int ret = thread_start(storage, desc);
	if (ret != OVE_OK) {
		return ret;
	}

	*handle = storage;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	if (handle->started) {
		/* Resume if suspended so it can finish */
		if (handle->state == OVE_THREAD_STATE_SUSPENDED) {
			handle->state = OVE_THREAD_STATE_READY;
			if (handle->suspend_inited) {
				nxsem_post(&handle->suspend_sem);
			}
		}
		/* Wait for thread to finish naturally (join) */
		if (handle->state != OVE_THREAD_STATE_TERMINATED) {
			nxsem_wait_uninterruptible(&handle->done_sem);
		}
	}

	if (first_thread == handle) {
		first_thread = NULL;
	}

	if (handle->suspend_inited) {
		nxsem_destroy(&handle->suspend_sem);
	}
	nxsem_destroy(&handle->done_sem);
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_THREAD
int ove_thread_create_(ove_thread_t *handle,
				const struct ove_thread_desc *desc)
{
	struct ove_thread *t;
	int ret;

	if (handle == NULL || desc == NULL || desc->entry == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (t == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(t, 0, sizeof(*t));

	ret = thread_start(t, desc);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(t);
		return ret;
	}

	*handle = t;
	return OVE_OK;
}

int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	ret = ove_thread_deinit(handle);
	OVE_BACKEND_FREE(handle);
	return ret;
}
#endif /* OVE_HEAP_THREAD */

ove_thread_t ove_thread_get_self(void)
{
	return tls_get_current();
}

void ove_thread_set_priority(ove_thread_t handle,
				      ove_prio_t prio)
{
	struct sched_param param;
	param.sched_priority = map_priority(prio);

	pid_t pid = (handle != NULL) ? handle->pid : getpid();
	sched_setparam(pid, &param);
}

void ove_thread_sleep_ms(uint32_t ms)
{
	usleep(ms * 1000U);
}

void ove_thread_yield(void)
{
	sched_yield();
}

void ove_thread_start_scheduler(void)
{
#ifdef __NuttX__
	/* Block until first thread exits (keeps init task alive).
	 * Threads were already created by ove_thread_init() via task_create;
	 * they begin running once this task yields or blocks. */
	if (first_thread != NULL) {
		nxsem_wait_uninterruptible(&first_thread->done_sem);
	}
#endif
	/* On Linux, POSIX threads run immediately — no scheduler to start */
}

void ove_thread_suspend(ove_thread_t handle)
{
	if (handle && handle->started) {
		if (!handle->suspend_inited) {
			nxsem_init(&handle->suspend_sem, 0, 0);
			handle->suspend_inited = 1;
		}
		kill(handle->pid, SIGUSR1);
		/* Wait briefly for the signal to be delivered */
		usleep(1000);
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	if (handle && handle->state == OVE_THREAD_STATE_SUSPENDED) {
		handle->state = OVE_THREAD_STATE_READY;
		nxsem_post(&handle->suspend_sem);
	}
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	/* NuttX: stack usage tracking not available via POSIX API.
	 * Returns 0 (unknown) rather than an error code. */
	(void)handle;
	return 0;
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	if (handle == NULL) {
		return OVE_THREAD_STATE_TERMINATED;
	}

#ifdef __NuttX__
	/* Quick liveness check — no procfs file I/O */
	if (handle->state == OVE_THREAD_STATE_RUNNING) {
		if (kill(handle->pid, 0) != 0) {
			return OVE_THREAD_STATE_TERMINATED;
		}
	}
#endif

	return handle->state;
}

int ove_thread_get_runtime_stats(ove_thread_t handle,
					  struct ove_thread_stats *stats)
{
	(void)handle;
	stats->runtime_us = 0;
	stats->cpu_percent_x100 = 0;
	return OVE_ERR_NOT_SUPPORTED;
}
