/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C Power Management Example — heap mode.
 *
 * Demonstrates the ove_pm_* API alongside heap-mode kernel object
 * creation: threads are spawned with ove_thread_create() inside
 * ove_main().  Pair with apps/c/zeroheap/example_pm/ which uses
 * OVE_THREAD_DEFINE_STATIC at file scope.
 */

#include "ove/ove.h"

static int battery_pct = 85;

static int sleep_until_stopped(ove_thread_t self, uint32_t duration_ms)
{
	while (duration_ms > 0 && !ove_thread_should_stop(self)) {
		uint32_t step_ms = duration_ms > 100 ? 100 : duration_ms;
		ove_thread_sleep_ms(step_ms);
		duration_ms -= step_ms;
	}
	return ove_thread_should_stop(self);
}

static void pm_notify(ove_pm_event_t event, ove_pm_state_t from, ove_pm_state_t to, void *user_data)
{
	(void)user_data;

	if (event == OVE_PM_EVENT_PRE_SLEEP) {
		OVE_LOG_INF("pm: preparing sleep %d -> %d", from, to);
	} else {
		OVE_LOG_INF("pm: woke %d -> %d", from, to);
	}
}

static ove_pm_state_t battery_policy(ove_pm_state_t current, uint32_t idle_ms,
				     uint32_t next_timeout_ms, void *user_data)
{
	int *batt = (int *)user_data;

	(void)current;
	(void)next_timeout_ms;

	if (*batt < 15) {
		if (idle_ms > 5)
			return OVE_PM_STATE_DEEP_SLEEP;
		return OVE_PM_STATE_STANDBY;
	}
	if (idle_ms < 10)
		return OVE_PM_STATE_ACTIVE;
	if (idle_ms < 1000)
		return OVE_PM_STATE_IDLE;
	if (idle_ms < 10000)
		return OVE_PM_STATE_STANDBY;
	return OVE_PM_STATE_DEEP_SLEEP;
}

static void sensor_thread(void *arg)
{
	(void)arg;
	ove_thread_t self = ove_thread_get_self();
	uint32_t reading = 0;

	OVE_LOG_INF("sensor: started");

	while (!ove_thread_should_stop(self)) {
		ove_pm_domain_request(OVE_PM_DOMAIN_SENSOR);
		ove_pm_activity();

		if (sleep_until_stopped(self, 50)) {
			ove_pm_domain_release(OVE_PM_DOMAIN_SENSOR);
			return;
		}
		reading += 17;

		OVE_LOG_INF("sensor: reading = %u", (unsigned int)(reading % 1000));

		ove_pm_domain_release(OVE_PM_DOMAIN_SENSOR);
		if (sleep_until_stopped(self, 5000))
			return;
	}
}

static void monitor_thread(void *arg)
{
	(void)arg;
	ove_thread_t self = ove_thread_get_self();
	struct ove_pm_stats stats;

	OVE_LOG_INF("monitor: started");

	while (!sleep_until_stopped(self, 10000)) {
		if (ove_pm_get_stats(&stats) == OVE_OK) {
			OVE_LOG_INF("=== Power Stats ===");
			OVE_LOG_INF("  active:  %u us (%u transitions)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_ACTIVE],
				    (unsigned int)stats.transition_count[OVE_PM_STATE_ACTIVE]);
			OVE_LOG_INF("  idle:    %u us (%u transitions)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_IDLE],
				    (unsigned int)stats.transition_count[OVE_PM_STATE_IDLE]);
			OVE_LOG_INF("  standby: %u us (%u transitions)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_STANDBY],
				    (unsigned int)stats.transition_count[OVE_PM_STATE_STANDBY]);
			OVE_LOG_INF("  deep:    %u us (%u transitions)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_DEEP_SLEEP],
				    (unsigned int)stats.transition_count[OVE_PM_STATE_DEEP_SLEEP]);
			OVE_LOG_INF("  active%%: %u.%02u%%",
				    (unsigned int)(stats.active_pct_x100 / 100),
				    (unsigned int)(stats.active_pct_x100 % 100));
		}

		if (battery_pct > 5)
			battery_pct -= 5;
		OVE_LOG_INF("battery: %d%%", battery_pct);
	}
}

void ove_main(void)
{
	OVE_LOG_INF("pm example (heap mode): init");

	const struct ove_pm_cfg pm_cfg = {
		.idle_threshold_ms = 50,
		.standby_threshold_ms = 5000,
		.deep_sleep_threshold_ms = 30000,
	};
	if (ove_pm_init(&pm_cfg) != OVE_OK) {
		OVE_LOG_ERR("Failed to init PM");
		return;
	}

	const struct ove_pm_wake_src btn_wake = {
		.type = OVE_PM_WAKE_GPIO,
		.gpio = {.port = 0, .pin = 13, .edge = OVE_GPIO_IRQ_FALLING},
	};
	ove_pm_wake_register(&btn_wake);

	const struct ove_pm_wake_src uart_wake = {
		.type = OVE_PM_WAKE_UART,
		.uart = {.instance = 0},
	};
	ove_pm_wake_register(&uart_wake);

	ove_pm_notify_register(pm_notify, NULL);
	ove_pm_set_policy(battery_policy, &battery_pct);
	ove_pm_set_budget(6000);

	ove_thread_t sensor;
	ove_thread_t monitor;
	if (ove_thread_create(&sensor, "sensor", sensor_thread, NULL, OVE_PRIO_NORMAL, 4096) !=
	    OVE_OK) {
		OVE_LOG_ERR("Failed to spawn sensor thread");
		ove_pm_deinit();
		return;
	}
	if (ove_thread_create(&monitor, "monitor", monitor_thread, NULL, OVE_PRIO_LOW, 4096) !=
	    OVE_OK) {
		OVE_LOG_ERR("Failed to spawn monitor thread");
		ove_thread_request_stop(sensor);
		ove_thread_destroy(sensor);
		ove_pm_deinit();
		return;
	}

	OVE_LOG_INF("pm example (heap mode): ready (battery=%d%%)", battery_pct);

	ove_run();

	ove_thread_request_stop(monitor);
	ove_thread_request_stop(sensor);
	ove_thread_destroy(monitor);
	ove_thread_destroy(sensor);
	ove_pm_deinit();
	OVE_LOG_INF("pm example (heap mode): shutdown");
}
