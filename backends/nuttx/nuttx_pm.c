/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PM

#include "ove/hal/hal_pm.h"
#include "ove/log.h"
#include "ove_backend_common.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>

#ifdef CONFIG_PM
#include <nuttx/power/pm.h>

/* Map oveRTOS states to NuttX PM states */
static enum pm_state_e to_nuttx_state(ove_pm_state_t state)
{
	switch (state) {
	case OVE_PM_STATE_IDLE:
		return PM_IDLE;
	case OVE_PM_STATE_STANDBY:
		return PM_STANDBY;
	case OVE_PM_STATE_DEEP_SLEEP:
		return PM_SLEEP;
	default:
		return PM_NORMAL;
	}
}
#endif /* CONFIG_PM */

/* Residency time per low-power state.  Each ove_hal_pm_enter_state()
 * blocks the calling thread (the pm idle poller) for this long, which
 * has two effects:
 *   1. NuttX's kernel-level idle path (up_idle / up_idlepm) gets to run
 *      while the poller is sleeping, executing real WFI in the chosen
 *      pm_state_e.
 *   2. The portable PM stats accumulate plausible time-in-state counts
 *      instead of zero, since update_stats() on wake sees a real delta.
 * Tuned to 1 s.  Without an authoritative "next-scheduled-wake" query
 * (NuttX user-space has no equivalent of FreeRTOS xNextTaskUnblockTime),
 * fixed residency is the only pacing knob.  Anything shorter blows up
 * the transition count: at 50 ms the poller emits ~30 transitions/sec
 * during a 5 s sensor sleep — the visible churn the user flagged.  At
 * 1 s we get ~2 transitions/sec, comparable to FreeRTOS tickless idle,
 * while still re-checking the policy frequently enough to honour the
 * 5 s standby and 30 s deep-sleep thresholds. */
#define PM_STATE_RESIDENCY_US 1000000

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
	uint32_t sleep_us = (expected_idle_ms == OVE_WAIT_FOREVER) ? PM_STATE_RESIDENCY_US
								   : expected_idle_ms * 1000U;

#ifdef CONFIG_PM
	pm_changestate(PM_IDLE_DOMAIN, to_nuttx_state(state));
	usleep(sleep_us);
	pm_changestate(PM_IDLE_DOMAIN, PM_NORMAL);
#else
	(void)state;
	usleep(sleep_us);
#endif
	return OVE_OK;
}

int ove_hal_pm_wake_arm(const struct ove_pm_wake_src *src)
{
	(void)src;
	return OVE_OK;
}

int ove_hal_pm_wake_disarm(const struct ove_pm_wake_src *src)
{
	(void)src;
	return OVE_OK;
}

int ove_hal_pm_domain_enable(ove_pm_domain_t domain)
{
	(void)domain;
	return OVE_OK;
}

int ove_hal_pm_domain_disable(ove_pm_domain_t domain)
{
	(void)domain;
	return OVE_OK;
}

uint32_t ove_hal_pm_get_next_timeout_ms(void)
{
	return OVE_WAIT_FOREVER;
}

void ove_hal_pm_idle_hook(void)
{
	ove_pm_idle_process();
}

/*
 * NuttX has no application-overridable idle hook (up_idle is in-kernel),
 * so drive the PM state machine from a low-priority polling thread.  The
 * thread yields between iterations so it only runs when no higher-prio
 * task is ready — effectively the same role FreeRTOS's idle task plays
 * for vApplicationIdleHook.
 *
 * When ove_pm_idle_process() decides to transition to a low-power state
 * it calls ove_hal_pm_enter_state(), which blocks the poller for
 * PM_STATE_RESIDENCY_US — that's where pacing lives.  When the poll
 * decides to stay ACTIVE no transition fires and we want the poller to
 * yield without burning CPU; sched_yield() lets a higher-priority task
 * run if any is ready, otherwise NuttX's idle task takes over.
 */
static pthread_t pm_thread;
static atomic_int pm_thread_running;

static void *pm_idle_thread(void *arg)
{
	(void)arg;
	while (atomic_load_explicit(&pm_thread_running, memory_order_acquire)) {
		ove_pm_idle_process();
		sched_yield();
	}
	return NULL;
}

int ove_hal_pm_setup(void)
{
	pthread_attr_t attr;
	struct sched_param sp;
	int rc;

	if (atomic_load_explicit(&pm_thread_running, memory_order_acquire))
		return OVE_OK;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 4096);
	sp.sched_priority = sched_get_priority_min(SCHED_RR);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setschedparam(&attr, &sp);

	atomic_store_explicit(&pm_thread_running, 1, memory_order_release);
	rc = pthread_create(&pm_thread, &attr, pm_idle_thread, NULL);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		atomic_store_explicit(&pm_thread_running, 0, memory_order_release);
		OVE_LOG_ERR("pm: failed to spawn idle thread (errno=%d)", rc);
		return OVE_ERR_NO_MEMORY;
	}
	return OVE_OK;
}

void ove_hal_pm_teardown(void)
{
	if (!atomic_load_explicit(&pm_thread_running, memory_order_acquire))
		return;
	atomic_store_explicit(&pm_thread_running, 0, memory_order_release);
	pthread_join(pm_thread, NULL);
}

#endif /* CONFIG_OVE_PM */
