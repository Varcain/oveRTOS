/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * POSIX simulator backend for ove/irq.h.
 *
 * There is no true interrupt model in the host POSIX backend. A thread-
 * local counter stands in for nested critical sections, and a separate
 * thread-local flag marks "currently inside a simulator ISR wrapper".
 * The simulator's HAL paths (e.g. timer SIGEV_THREAD dispatchers, GPIO
 * IRQ injection) set the ISR flag while invoking user callbacks; the
 * async runtime reads ove_is_in_isr() to decide between thread- and
 * ISR-context wake variants.
 *
 * The "critical section" implementation does not actually suspend
 * preemption — pthreads can't. Code that relies on critical_section::with
 * for correctness on hosted targets is using it as a coarse exclusion
 * mechanism only; the embassy queues we wrap with it are themselves
 * thread-safe at a finer grain.
 */

#include "ove/irq.h"
#include "ove/types.h"
#include "ove_config.h"

#ifdef CONFIG_OVE_ASYNC

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Per-thread state. Initialiser-free — POSIX zeros TLS for us.
 *
 * lock_depth: nesting count for ove_irq_lock / ove_irq_unlock pairs.
 *             Non-zero means "in critical section"; this is informational
 *             only (we have no real interrupts to mask on the host).
 * in_isr:     set by simulator entry wrappers before invoking user
 *             callbacks, cleared on return. Read by ove_is_in_isr.
 */
static _Thread_local uint64_t s_lock_depth;
static _Thread_local int s_in_isr;

/*
 * Single global mutex used as a coarse "interrupt mask" across host
 * threads. Acquired on the outermost ove_irq_lock and released on the
 * outermost ove_irq_unlock. This gives the async runtime a consistent
 * cross-thread serialisation point for its time-queue and waker
 * registration, which is what critical_section::with assumes on real
 * embedded targets where interrupts are actually disabled.
 *
 * Recursive so nested ove_irq_lock from the same thread (e.g. via
 * critical_section::with inside another critical section) doesn't
 * deadlock. Initialised by pthread_once on first lock; the
 * PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP form is a glibc extension
 * that isn't portable, so we use the runtime path.
 */
static pthread_mutex_t s_irq_mtx;
static pthread_once_t s_irq_mtx_once = PTHREAD_ONCE_INIT;

static void s_irq_mtx_init(void)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&s_irq_mtx, &attr);
	pthread_mutexattr_destroy(&attr);
}

ove_irq_key_t ove_irq_lock(void)
{
	pthread_once(&s_irq_mtx_once, s_irq_mtx_init);
	pthread_mutex_lock(&s_irq_mtx);
	return ++s_lock_depth;
}

void ove_irq_unlock(ove_irq_key_t key)
{
	(void)key;
	if (s_lock_depth > 0) {
		s_lock_depth--;
	}
	pthread_mutex_unlock(&s_irq_mtx);
}

bool ove_is_in_isr(void)
{
	return s_in_isr != 0;
}

/*
 * Simulator-side helpers — not part of the public API. Used by the
 * POSIX HAL layer to bracket callback dispatch from "ISR" contexts
 * (timer SIGEV_THREAD, GPIO injection threads, etc.) so user code that
 * queries ove_is_in_isr() inside those callbacks gets `true`.
 */
void posix_irq_enter_isr(void)
{
	s_in_isr = 1;
}

void posix_irq_leave_isr(void)
{
	s_in_isr = 0;
}

#endif /* CONFIG_OVE_ASYNC */
