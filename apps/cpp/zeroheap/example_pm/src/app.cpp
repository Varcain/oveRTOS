/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Power Management Example — zero-heap mode.
 *
 * Threads are file-scope `ove::Thread<>` instances; in zero-heap mode
 * the wrapper carries the kernel storage and stack inline as struct
 * members.  Static constructors run after pm::init has registered its
 * subsystem mutex (handled by static-initialisation ordering inside
 * OVE_MAIN's hook), so domain_request from these threads is valid by
 * the time they execute.
 */

#include <ove/ove.hpp>

namespace pm = ove::pm;

static int battery_pct = 85;

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
	int *batt = static_cast<int *>(user_data);

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

static void sensor_thread(void *)
{
	uint32_t reading = 0;

	OVE_LOG_INF("sensor: started");

	while (true) {
		(void)pm::domain_request(OVE_PM_DOMAIN_SENSOR);
		pm::activity();

		ove::this_thread::sleep_ms(50);
		reading += 17;
		OVE_LOG_INF("sensor: reading = %u", reading % 1000);

		(void)pm::domain_release(OVE_PM_DOMAIN_SENSOR);
		ove::this_thread::sleep_ms(5000);
	}
}

static void monitor_thread(void *)
{
	OVE_LOG_INF("monitor: started");

	while (true) {
		ove::this_thread::sleep_ms(10000);

		if (auto stats = pm::get_stats()) {
			OVE_LOG_INF("=== Power Stats ===");
			OVE_LOG_INF("  active:  %u us (%u transitions)",
				    (unsigned)stats->time_in_state_us[OVE_PM_STATE_ACTIVE],
				    stats->transition_count[OVE_PM_STATE_ACTIVE]);
			OVE_LOG_INF("  idle:    %u us (%u transitions)",
				    (unsigned)stats->time_in_state_us[OVE_PM_STATE_IDLE],
				    stats->transition_count[OVE_PM_STATE_IDLE]);
			OVE_LOG_INF("  standby: %u us (%u transitions)",
				    (unsigned)stats->time_in_state_us[OVE_PM_STATE_STANDBY],
				    stats->transition_count[OVE_PM_STATE_STANDBY]);
			OVE_LOG_INF("  deep:    %u us (%u transitions)",
				    (unsigned)stats->time_in_state_us[OVE_PM_STATE_DEEP_SLEEP],
				    stats->transition_count[OVE_PM_STATE_DEEP_SLEEP]);
			OVE_LOG_INF("  active%%: %u.%02u%%", stats->active_pct_x100 / 100,
				    stats->active_pct_x100 % 100);
		}

		if (battery_pct > 5)
			battery_pct -= 5;
		OVE_LOG_INF("battery: %d%%", battery_pct);
	}
}

OVE_MAIN()
{
	OVE_LOG_INF("pm example (zero-heap mode): init");

	const pm::Cfg cfg{
		.idle_threshold_ms = 50,
		.standby_threshold_ms = 5000,
		.deep_sleep_threshold_ms = 30000,
	};
	if (!pm::init(cfg)) {
		OVE_LOG_ERR("PM init failed");
		return;
	}

	pm::WakeSrc btn{};
	btn.type = OVE_PM_WAKE_GPIO;
	btn.gpio = {.port = 0, .pin = 13, .edge = OVE_GPIO_IRQ_FALLING};
	(void)pm::wake_register(btn);

	pm::WakeSrc uart{};
	uart.type = OVE_PM_WAKE_UART;
	uart.uart = {.instance = 0};
	(void)pm::wake_register(uart);

	(void)pm::notify_register(pm_notify);
	(void)pm::set_policy(battery_policy, &battery_pct);
	(void)pm::set_budget(6000);

	// Function-static `ove::Thread<>` instances: in zero-heap mode the
	// wrapper embeds the kernel storage and stack inline.  Constructed
	// here (after pm::init) rather than at file scope so the PM mutex
	// is initialised before sensor_thread's first domain_request.
	static ove::Thread<4096> sensor_th(sensor_thread, nullptr, OVE_PRIO_NORMAL, "sensor");
	static ove::Thread<4096> monitor_th(monitor_thread, nullptr, OVE_PRIO_LOW, "monitor");
	(void)sensor_th;
	(void)monitor_th;

	OVE_LOG_INF("pm example (zero-heap mode): ready (battery=%d%%)", battery_pct);

	ove::run();

	OVE_LOG_INF("pm example (zero-heap mode): shutdown");
}
