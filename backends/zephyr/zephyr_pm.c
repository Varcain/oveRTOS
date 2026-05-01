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

#include <zephyr/kernel.h>

#ifdef CONFIG_PM
#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#endif

/* Map oveRTOS states to Zephyr PM states */
#ifdef CONFIG_PM
static enum pm_state to_zephyr_state(ove_pm_state_t state)
{
	switch (state) {
	case OVE_PM_STATE_IDLE:
		return PM_STATE_RUNTIME_IDLE;
	case OVE_PM_STATE_STANDBY:
		return PM_STATE_SUSPEND_TO_IDLE;
	case OVE_PM_STATE_DEEP_SLEEP:
		return PM_STATE_STANDBY;
	default:
		return PM_STATE_ACTIVE;
	}
}
#endif

/* Residency time per low-power state.  Each ove_hal_pm_enter_state()
 * blocks the calling thread (the pm idle poller) for this long, which
 * has two effects:
 *   1. Zephyr's kernel idle path runs in the forced pm_state during the
 *      sleep, executing real WFI / deeper-sleep entry as configured.
 *   2. The portable PM stats accumulate plausible time-in-state counts
 *      since update_stats() on wake sees a non-zero delta.
 * Tuned to 1 s.  Without an authoritative "next-scheduled-wake" query
 * exposed to the application thread, fixed residency is the only pacing
 * knob.  k_msleep(1) per poll iteration emits ~30 transitions/sec
 * during a 5 s sensor sleep — visible churn in the stats.  At 1 s we
 * get ~2 transitions/sec, comparable to FreeRTOS tickless idle, while
 * still re-checking the policy frequently enough to honour the 5 s
 * standby and 30 s deep-sleep thresholds. */
#define PM_STATE_RESIDENCY_MS 1000

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
	int32_t sleep_ms = (expected_idle_ms == OVE_WAIT_FOREVER)
				   ? PM_STATE_RESIDENCY_MS
				   : (int32_t)expected_idle_ms;

#ifdef CONFIG_PM
	struct pm_state_info info = {
		.state = to_zephyr_state(state),
		.substate_id = 0,
		.min_residency_us = 0,
		.exit_latency_us = 0,
	};
	pm_state_force(0, &info);
	k_msleep(sleep_ms);
#else
	(void)state;
	k_msleep(sleep_ms);
#endif
	return OVE_OK;
}

int ove_hal_pm_wake_arm(const struct ove_pm_wake_src *src)
{
	/* GPIO wake: Zephyr GPIO interrupts configured via ove_gpio layer.
	 * Timer/UART/RTC: handled by Zephyr device drivers natively.
	 */
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
#ifdef CONFIG_PM_DEVICE
	/* Board-specific: call pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME)
	 * for devices in this domain.  Requires a board-level domain→device map.
	 */
	(void)domain;
#else
	(void)domain;
#endif
	return OVE_OK;
}

int ove_hal_pm_domain_disable(ove_pm_domain_t domain)
{
#ifdef CONFIG_PM_DEVICE
	(void)domain;
#else
	(void)domain;
#endif
	return OVE_OK;
}

uint32_t ove_hal_pm_get_next_timeout_ms(void)
{
	/* Return OVE_WAIT_FOREVER — Zephyr's kernel idle path handles
	 * the actual next-timeout calculation internally. */
	return OVE_WAIT_FOREVER;
}

void ove_hal_pm_idle_hook(void)
{
	ove_pm_idle_process();
}

/*
 * Zephyr's idle hooks (k_idle) aren't directly application-overridable
 * for arbitrary callbacks, so we drive the PM state machine from a
 * dedicated low-priority polling thread.  CONFIG_NUM_PREEMPT_PRIORITIES-1
 * is the lowest preemptible priority, so we sit just above the kernel
 * idle task — same effective role.
 *
 * When ove_pm_idle_process() decides to transition to a low-power state
 * it calls ove_hal_pm_enter_state(), which blocks the poller for
 * PM_STATE_RESIDENCY_MS — that's where pacing lives.  When the poll
 * decides to stay ACTIVE no transition fires and we just yield.
 */
#define PM_IDLE_STACK_SIZE 4096
K_THREAD_STACK_DEFINE(pm_idle_stack, PM_IDLE_STACK_SIZE);
static struct k_thread pm_idle_thread_data;
static k_tid_t pm_idle_tid;
static atomic_t pm_idle_running;

static void pm_idle_entry(void *p1, void *p2, void *p3)
{
	(void)p1;
	(void)p2;
	(void)p3;
	while (atomic_get(&pm_idle_running)) {
		ove_pm_idle_process();
		k_yield();
	}
}

int ove_hal_pm_setup(void)
{
	if (atomic_get(&pm_idle_running))
		return OVE_OK;

	atomic_set(&pm_idle_running, 1);
	pm_idle_tid = k_thread_create(&pm_idle_thread_data, pm_idle_stack,
				      K_THREAD_STACK_SIZEOF(pm_idle_stack),
				      pm_idle_entry, NULL, NULL, NULL,
				      K_LOWEST_APPLICATION_THREAD_PRIO,
				      0, K_NO_WAIT);
	if (!pm_idle_tid) {
		atomic_set(&pm_idle_running, 0);
		OVE_LOG_ERR("pm: failed to spawn idle thread");
		return OVE_ERR_NO_MEMORY;
	}
	k_thread_name_set(pm_idle_tid, "ove_pm_idle");
	return OVE_OK;
}

void ove_hal_pm_teardown(void)
{
	if (!atomic_get(&pm_idle_running))
		return;
	atomic_set(&pm_idle_running, 0);
	k_thread_join(pm_idle_tid, K_FOREVER);
	pm_idle_tid = NULL;
}

#endif /* CONFIG_OVE_PM */
