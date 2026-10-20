/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host-side stand-in for the NuttX surface the suspend/resume handshake of
 * backends/nuttx/nuttx_thread.c touches, plus the little task model the
 * regression test drives.
 *
 * The handshake code itself is NOT re-implemented here: it is extracted
 * verbatim out of backends/nuttx/nuttx_thread.c at build time (see
 * tests/cmake/extract_nuttx_suspend_block.py) and compiled against this
 * header.  This file only provides what that code needs:
 *
 *   - the NuttX semaphore API (nxsem_*) on top of the host's POSIX
 *     semaphores (the NuttX storage header declares `sem_t` from
 *     <semaphore.h>, which is the host header on this platform),
 *   - the thread state store (SET_STATE),
 *   - the per-task pointer the backend keeps in the TLS,
 *   - the signal action installation, and
 *   - kill(SIGUSR1), which NuttX *queues* instead of delivering it inline.
 *
 * The NuttX rules the model reproduces (and where they come from):
 *
 *   1. A signal action is queued, never executed inline:
 *      sched/signal/sig_dispatch.c:602 -> nxsig_queue_action() ->
 *      arch/arm/src/armv7-m/arm_schedulesigaction.c:61 ("This operation
 *      should not cause the task to be unblocked nor should it cause any
 *      immediate execution of sigdeliver").  The action runs at the
 *      recipient's next dispatch.
 *   2. Without an installed action the signal is dropped silently:
 *      sched/signal/sig_dispatch.c:117-127 ("There is no default action").
 */

#ifndef OVE_TEST_NUTTX_SUSPEND_SHIM_H
#define OVE_TEST_NUTTX_SUSPEND_SHIM_H

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

/* ── the NuttX semaphore API ────────────────────────────────────────── */

static inline int nxsem_init(sem_t *sem, int pshared, int32_t value)
{
	return sem_init(sem, pshared, value);
}

static inline int nxsem_post(sem_t *sem)
{
	return sem_post(sem);
}

/* A test can hold a woken task back before it returns from the wait, as
 * if it had a lower priority than the task that woke it and were not
 * dispatched yet; shim_wakes counts the wake-ups. */
extern volatile int shim_hold_after_wake;
extern volatile int shim_wakes;

static inline int nxsem_wait(sem_t *sem)
{
	int r = sem_wait(sem);

	__atomic_add_fetch(&shim_wakes, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&shim_hold_after_wake, __ATOMIC_SEQ_CST))
		usleep(100);
	return r;
}

/* The non-blocking wait: it takes a token if there is one and fails with
 * -EAGAIN otherwise.  The backend drains the suspend semaphore before it
 * posts a token, so that at most one token is outstanding. */
static inline int nxsem_trywait(sem_t *sem)
{
	if (sem_trywait(sem) == 0) {
		return 0;
	}
	return -EAGAIN;
}

static inline int nxsem_destroy(sem_t *sem)
{
	return sem_destroy(sem);
}

/* ── the thread state store ─────────────────────────────────────────── */

/* The NuttX backend's SET_STATE() also drives the scheduler-note
 * observation; this test does not model the note driver, so only the
 * state itself is stored (with the same release semantics). */
#define SET_STATE(t, s)                                               \
	do {                                                          \
		__atomic_store_n(&(t)->state, (s), __ATOMIC_RELEASE); \
	} while (0)

/* ── the per-task pointer (the backend's TLS) ───────────────────────── */

extern __thread struct ove_thread *sim_tls_current;

static inline struct ove_thread *tls_get_current(void)
{
	return sim_tls_current;
}

static inline void tls_set_current(struct ove_thread *t)
{
	sim_tls_current = t;
}

/* ── the signal action installation ─────────────────────────────────── */

/* True once the task installed its SIGUSR1 action.  NuttX drops a signal
 * whose action is not installed yet (sig_dispatch.c:117), so the model has
 * to know about the installation. */
extern int sim_sigusr1_installed;

static inline void install_sigusr1_handler(void)
{
	sim_sigusr1_installed = 1;
}

/* ── the queued signal ──────────────────────────────────────────────── */

/* The model's kill(): queues the SIGUSR1 action for the task with the
 * given pid.  The action is *not* delivered here - the test delivers it
 * at a dispatch point of its choosing. */
int sim_kill(pid_t pid, int sig);

#define kill(pid, sig) sim_kill((pid), (sig))

#endif /* OVE_TEST_NUTTX_SUSPEND_SHIM_H */
