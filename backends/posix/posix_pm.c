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
#include "posix_sleep.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

static const char *state_name(ove_pm_state_t state)
{
	switch (state) {
	case OVE_PM_STATE_ACTIVE:
		return "ACTIVE";
	case OVE_PM_STATE_IDLE:
		return "IDLE";
	case OVE_PM_STATE_STANDBY:
		return "STANDBY";
	case OVE_PM_STATE_DEEP_SLEEP:
		return "DEEP_SLEEP";
	default:
		return "UNKNOWN";
	}
}

static const char *wake_type_name(ove_pm_wake_type_t type)
{
	switch (type) {
	case OVE_PM_WAKE_GPIO:
		return "GPIO";
	case OVE_PM_WAKE_TIMER:
		return "TIMER";
	case OVE_PM_WAKE_UART:
		return "UART";
	case OVE_PM_WAKE_RTC:
		return "RTC";
	default:
		return "UNKNOWN";
	}
}

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
	OVE_LOG_INF("pm: [POSIX] enter %s (expected %u ms)", state_name(state), expected_idle_ms);
	/* Brief sleep to simulate state transition for test observability. */
	posix_sleep_ns(1000000ULL);
	return OVE_OK;
}

int ove_hal_pm_wake_arm(const struct ove_pm_wake_src *src)
{
	OVE_LOG_DBG("pm: [POSIX] arm wake %s", wake_type_name(src->type));
	return OVE_OK;
}

int ove_hal_pm_wake_disarm(const struct ove_pm_wake_src *src)
{
	OVE_LOG_DBG("pm: [POSIX] disarm wake %s", wake_type_name(src->type));
	return OVE_OK;
}

int ove_hal_pm_domain_enable(ove_pm_domain_t domain)
{
	OVE_LOG_DBG("pm: [POSIX] domain %d enabled", domain);
	return OVE_OK;
}

int ove_hal_pm_domain_disable(ove_pm_domain_t domain)
{
	OVE_LOG_DBG("pm: [POSIX] domain %d disabled", domain);
	return OVE_OK;
}

uint32_t ove_hal_pm_get_next_timeout_ms(void)
{
	return OVE_PM_NO_TIMEOUT;
}

void ove_hal_pm_idle_hook(void)
{
	ove_pm_idle_process();
}

/* POSIX has no kernel idle hook, so drive ove_pm_idle_process() from a
 * dedicated polling thread.  Sleep 1 ms between polls to keep host CPU
 * usage minimal while still giving the state machine a chance to react. */
static pthread_t pm_thread;
static atomic_int pm_thread_running;

static void *pm_idle_thread(void *arg)
{
	(void)arg;
	while (atomic_load_explicit(&pm_thread_running, memory_order_acquire)) {
		ove_pm_idle_process();
		usleep(1000);
	}
	return NULL;
}

int ove_hal_pm_setup(void)
{
	if (atomic_load_explicit(&pm_thread_running, memory_order_acquire))
		return OVE_OK;
	atomic_store_explicit(&pm_thread_running, 1, memory_order_release);
	if (pthread_create(&pm_thread, NULL, pm_idle_thread, NULL) != 0) {
		atomic_store_explicit(&pm_thread_running, 0, memory_order_release);
		OVE_LOG_ERR("pm: failed to spawn POSIX idle thread");
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
