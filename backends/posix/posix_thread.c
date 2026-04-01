/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <semaphore.h>
#include <string.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <string.h>

static __thread struct ove_thread *tls_current;
static struct ove_thread *first_thread;

static void sigusr1_handler(int sig)
{
	(void)sig;
	struct ove_thread *t = tls_current;
	if (t) {
		t->state = OVE_THREAD_STATE_SUSPENDED;
		/* Block until resumed via sem_post */
		while (t->state == OVE_THREAD_STATE_SUSPENDED) {
			sem_wait(&t->suspend_sem);
		}
	}
}

static void *thread_wrapper(void *arg)
{
	struct ove_thread *t = arg;
	tls_current = t;

	/* Install signal handler for suspend */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigusr1_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);

	t->state = OVE_THREAD_STATE_RUNNING;
	t->entry(t->arg);
	t->state = OVE_THREAD_STATE_TERMINATED;
	return NULL;
}

int ove_thread_init(ove_thread_t *handle,
			ove_thread_storage_t *storage,
			const struct ove_thread_desc *desc)
{
	if (!handle || !storage || !desc || !desc->entry) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_thread *t = (struct ove_thread *)storage;
	memset(t, 0, sizeof(*t));

	t->entry = desc->entry;
	t->arg = desc->arg;
	t->state = OVE_THREAD_STATE_READY;
	sem_init(&t->suspend_sem, 0, 0);

	if (pthread_create(&t->tid, NULL, thread_wrapper, t) != 0) {
		sem_destroy(&t->suspend_sem);
		return OVE_ERR_NO_MEMORY;
	}

	t->started = 1;

	if (first_thread == NULL) {
		first_thread = t;
	}

	*handle = t;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	struct ove_thread *t = handle;
	if (t->started) {
		if (t->state == OVE_THREAD_STATE_SUSPENDED) {
			t->state = OVE_THREAD_STATE_READY;
			sem_post(&t->suspend_sem);
		}
		pthread_join(t->tid, NULL);
	}
	if (first_thread == t) {
		first_thread = NULL;
	}
	sem_destroy(&t->suspend_sem);
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_thread_create_(ove_thread_t *handle,
			   const struct ove_thread_desc *desc)
{
	if (!handle || !desc || !desc->entry) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_thread *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(t, 0, sizeof(*t));

	t->entry = desc->entry;
	t->arg = desc->arg;
	t->state = OVE_THREAD_STATE_READY;
	sem_init(&t->suspend_sem, 0, 0);

	if (pthread_create(&t->tid, NULL, thread_wrapper, t) != 0) {
		sem_destroy(&t->suspend_sem);
		OVE_BACKEND_FREE(t);
		return OVE_ERR_NO_MEMORY;
	}

	t->started = 1;

	if (first_thread == NULL) {
		first_thread = t;
	}

	*handle = t;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret) return ret;

	struct ove_thread *t = handle;
	if (t->started) {
		/* Resume if suspended so it can finish */
		if (t->state == OVE_THREAD_STATE_SUSPENDED) {
			t->state = OVE_THREAD_STATE_READY;
			sem_post(&t->suspend_sem);
		}
		pthread_join(t->tid, NULL);
	}
	if (first_thread == t) {
		first_thread = NULL;
	}
	sem_destroy(&t->suspend_sem);
	OVE_BACKEND_FREE(t);
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

ove_thread_t ove_thread_get_self(void)
{
	return tls_current;
}

void ove_thread_set_priority(ove_thread_t handle,
				 ove_prio_t prio)
{
	(void)handle;
	(void)prio;
	/* POSIX thread priorities require root; no-op for stub */
}

void ove_thread_sleep_ms(uint32_t ms)
{
	usleep((useconds_t)ms * 1000);
}

void ove_thread_yield(void)
{
	sched_yield();
}

static void sigint_handler(int sig)
{
	(void)sig;
	_exit(0);
}

void ove_thread_start_scheduler(void)
{
	/* Allow Ctrl-C to terminate the process cleanly.  Without this,
	 * SIGINT interrupts pthread_join but the spawned threads keep the
	 * process alive because they are blocked in sleep/recv loops. */
	signal(SIGINT, sigint_handler);

	/* Block the main thread by joining the first app thread.
	 * POSIX threads run immediately, but the caller (ove_app_run)
	 * needs to block here to keep the process alive. */
	if (first_thread != NULL) {
		pthread_join(first_thread->tid, NULL);
	}
}

void ove_thread_suspend(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t && t->started) {
		pthread_kill(t->tid, SIGUSR1);
		/* Wait briefly for the signal to be delivered */
		usleep(1000);
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t && t->state == OVE_THREAD_STATE_SUSPENDED) {
		t->state = OVE_THREAD_STATE_READY;
		sem_post(&t->suspend_sem);
	}
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	(void)handle;
	return 0;
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	if (t) {
		return t->state;
	}
	return OVE_THREAD_STATE_UNKNOWN;
}

int ove_thread_get_runtime_stats(ove_thread_t handle,
				     struct ove_thread_stats *stats)
{
	(void)handle;
	if (stats != NULL) {
		stats->runtime_us = 0;
		stats->cpu_percent_x100 = 0;
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats) return OVE_ERR_INVALID_PARAM;

#ifdef __GLIBC__
	struct mallinfo2 mi = mallinfo2();
	stats->total     = mi.arena;
	stats->used      = mi.uordblks;
	stats->free      = mi.fordblks;
	stats->peak_used = 0; /* glibc doesn't track peak */
#else
	memset(stats, 0, sizeof(*stats));
#endif
	return OVE_OK;
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count,
		    size_t *actual_count)
{
	/* POSIX has no standard thread enumeration.
	 * Return an empty list — the dashboard will show "no data". */
	if (actual_count)
		*actual_count = 0;
	(void)out;
	(void)max_count;
	return OVE_OK;
}
