/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_PROTECTED)

#include "ove/protected.h"

#include <setjmp.h>
#include <signal.h>
#include <string.h>

/*
 * Host fault containment.
 *
 * The process MMU enforces memory protection; a SIGSEGV/SIGBUS during a
 * protected run is converted into a contained fault via siglongjmp back to
 * ove_ptask_run. The SIGSEGV/SIGBUS dispositions are installed for the
 * duration of the run and restored afterwards, so the containment is robust
 * against handlers installed by surrounding code (e.g. the test harness or a
 * sanitizer); faults outside the run window are chained to the saved handler.
 *
 * The arm flag, jump buffer and saved dispositions are thread-local. Signal
 * dispositions are a process-global resource, so a host protected run must not
 * overlap another on a different thread (the target backend has no such
 * restriction).
 */

static __thread sigjmp_buf t_jmp;
static __thread volatile sig_atomic_t t_armed;
static __thread struct sigaction t_prev_segv;
static __thread struct sigaction t_prev_bus;
static volatile unsigned long g_fault_count;

static void chain_prev(int sig, siginfo_t *info, void *uc)
{
	const struct sigaction *prev = (sig == SIGBUS) ? &t_prev_bus : &t_prev_segv;
	if (prev->sa_flags & SA_SIGINFO) {
		if (prev->sa_sigaction) {
			prev->sa_sigaction(sig, info, uc);
			return;
		}
	} else if (prev->sa_handler && prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
		prev->sa_handler(sig);
		return;
	}
	/* Default / ignore: restore the default action and let it re-fault. */
	(void)signal(sig, SIG_DFL);
}

static void prot_handler(int sig, siginfo_t *info, void *uc)
{
	if (t_armed) {
		t_armed = 0;
		__atomic_fetch_add(&g_fault_count, 1, __ATOMIC_RELAXED);
		siglongjmp(t_jmp, 1);
	}
	chain_prev(sig, info, uc);
}

int ove_ptask_run(ove_ptask_fn entry, void *arg, ove_ptask_result_t *result)
{
	if (!entry)
		return OVE_ERR_INVALID_PARAM;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = prot_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigaction(SIGSEGV, &sa, &t_prev_segv);
	sigaction(SIGBUS, &sa, &t_prev_bus);

	ove_ptask_result_t r;
	if (sigsetjmp(t_jmp, 1) == 0) {
		t_armed = 1;
		entry(arg);
		t_armed = 0;
		r = OVE_PTASK_OK;
	} else {
		/* Returned here from prot_handler via siglongjmp. */
		r = OVE_PTASK_FAULT;
	}
	t_armed = 0;

	sigaction(SIGSEGV, &t_prev_segv, NULL);
	sigaction(SIGBUS, &t_prev_bus, NULL);

	if (result)
		*result = r;
	return OVE_OK;
}

unsigned long ove_ptask_fault_count(void)
{
	return __atomic_load_n(&g_fault_count, __ATOMIC_RELAXED);
}

#endif /* CONFIG_OVE_PROTECTED */
