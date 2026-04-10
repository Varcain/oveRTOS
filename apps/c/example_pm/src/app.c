/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C Power Management Example
 *
 * Demonstrates the ove_pm_* API:
 *   - Sleep state machine with automatic idle transitions
 *   - Peripheral power domain reference counting
 *   - Wake source registration (GPIO button + UART)
 *   - Custom power policy (battery-aware)
 *   - Transition notifications for driver prepare/restore
 *   - Runtime power statistics reporting
 *
 * Architecture:
 *   sensor_thread  — periodically reads sensor, requests/releases domain
 *   monitor_thread — prints power stats every 10 seconds
 *   PM subsystem   — auto-sleeps when idle via default policy
 */

#include "ove/ove.h"
#include <stdio.h>

/* --- Simulated battery level --- */

static int battery_pct = 85;

/* --- PM transition notifier --- */

static void pm_notify(ove_pm_event_t event, ove_pm_state_t from,
		       ove_pm_state_t to, void *user_data)
{
	(void)user_data;

	if (event == OVE_PM_EVENT_PRE_SLEEP) {
		OVE_LOG_INF("pm: preparing sleep %d -> %d", from, to);
		/* Flush UART buffers, save ADC calibration, etc. */
	} else {
		OVE_LOG_INF("pm: woke %d -> %d", from, to);
		/* Reconfigure clocks, reinit peripherals if needed */
	}
}

/* --- Battery-aware power policy --- */

static ove_pm_state_t battery_policy(ove_pm_state_t current,
				     uint32_t idle_ms,
				     uint32_t next_timeout_ms,
				     void *user_data)
{
	int *batt = (int *)user_data;

	(void)current;
	(void)next_timeout_ms;

	/* Aggressive sleep when battery is critically low */
	if (*batt < 15) {
		if (idle_ms > 5)
			return OVE_PM_STATE_DEEP_SLEEP;
		return OVE_PM_STATE_STANDBY;
	}

	/* Normal thresholds */
	if (idle_ms < 10)
		return OVE_PM_STATE_ACTIVE;
	if (idle_ms < 1000)
		return OVE_PM_STATE_IDLE;
	if (idle_ms < 10000)
		return OVE_PM_STATE_STANDBY;
	return OVE_PM_STATE_DEEP_SLEEP;
}

/* --- Sensor thread: periodic read with domain management --- */

static void sensor_thread(void *arg)
{
	(void)arg;
	uint32_t reading = 0;

	OVE_LOG_INF("sensor: started");

	while (1) {
		/* Power on sensor domain */
		ove_pm_domain_request(OVE_PM_DOMAIN_SENSOR);
		ove_pm_activity();

		/* Simulate sensor read */
		ove_thread_sleep_ms(50);
		reading += 17;

		OVE_LOG_INF("sensor: reading = %u", reading % 1000);

		/* Power off sensor domain when done */
		ove_pm_domain_release(OVE_PM_DOMAIN_SENSOR);

		/* Sleep 5s — PM can enter low-power during this */
		ove_thread_sleep_ms(5000);
	}
}

/* --- Monitor thread: print power statistics --- */

static void monitor_thread(void *arg)
{
	(void)arg;
	struct ove_pm_stats stats;

	OVE_LOG_INF("monitor: started");

	while (1) {
		ove_thread_sleep_ms(10000);

		if (ove_pm_get_stats(&stats) == OVE_OK) {
			OVE_LOG_INF("=== Power Stats ===");
			OVE_LOG_INF("  active:  %u us (%u transitions)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_ACTIVE],
				    stats.transition_count[OVE_PM_STATE_ACTIVE]);
			OVE_LOG_INF("  idle:    %u us (%u transitions)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_IDLE],
				    stats.transition_count[OVE_PM_STATE_IDLE]);
			OVE_LOG_INF("  standby: %u us (%u transitions)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_STANDBY],
				    stats.transition_count[OVE_PM_STATE_STANDBY]);
			OVE_LOG_INF("  deep:    %u us (%u transitions)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_DEEP_SLEEP],
				    stats.transition_count[OVE_PM_STATE_DEEP_SLEEP]);
			OVE_LOG_INF("  active%%: %u.%02u%%",
				    stats.active_pct_x100 / 100,
				    stats.active_pct_x100 % 100);
		}

		/* Simulate battery drain */
		if (battery_pct > 5)
			battery_pct -= 5;
		OVE_LOG_INF("battery: %d%%", battery_pct);
	}
}

/* --- App entry point --- */

void ove_main(void)
{
	int ret;
	ove_thread_t th;

	OVE_LOG_INF("pm example: init");

	/* Initialize PM subsystem */
	struct ove_pm_cfg pm_cfg = {
		.idle_threshold_ms       = 50,
		.standby_threshold_ms    = 5000,
		.deep_sleep_threshold_ms = 30000,
	};
	ret = ove_pm_init(&pm_cfg);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to init PM: %d", ret);
		return;
	}

	/* Register wake sources */
	struct ove_pm_wake_src btn_wake = {
		.type = OVE_PM_WAKE_GPIO,
		.gpio = { .port = 0, .pin = 13, .edge = OVE_GPIO_IRQ_FALLING },
	};
	ove_pm_wake_register(&btn_wake);

	struct ove_pm_wake_src uart_wake = {
		.type = OVE_PM_WAKE_UART,
		.uart = { .instance = 0 },
	};
	ove_pm_wake_register(&uart_wake);

	/* Register transition notifier */
	ove_pm_notify_register(pm_notify, NULL);

	/* Install battery-aware policy */
	ove_pm_set_policy(battery_policy, &battery_pct);

	/* Set power budget target: 60% low-power */
	ove_pm_set_budget(6000);

	/* Create sensor thread */
	{
		struct ove_thread_desc desc = {
			.name     = "sensor",
			.entry    = sensor_thread,
			.arg      = NULL,
			.priority = OVE_PRIO_NORMAL,
		};
		ret = ove_thread_create(&th, 4096, &desc);
		if (ret != OVE_OK) {
			OVE_LOG_ERR("Failed to create sensor thread: %d", ret);
			return;
		}
	}

	/* Create monitor thread */
	{
		struct ove_thread_desc desc = {
			.name     = "monitor",
			.entry    = monitor_thread,
			.arg      = NULL,
			.priority = OVE_PRIO_LOW,
		};
		ret = ove_thread_create(&th, 4096, &desc);
		if (ret != OVE_OK) {
			OVE_LOG_ERR("Failed to create monitor thread: %d", ret);
			return;
		}
	}

	OVE_LOG_INF("pm example: ready (battery=%d%%)", battery_pct);

	ove_run();

	/* Cleanup (only reached on POSIX) */
	ove_pm_deinit();
	OVE_LOG_INF("pm example: shutdown");
}
