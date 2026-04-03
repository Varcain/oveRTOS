/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Power Management Example
 *
 * Demonstrates the ove::pm namespace:
 *   - Sleep state machine with auto-idle
 *   - Peripheral power domain reference counting
 *   - Wake source registration
 *   - Custom battery-aware power policy
 *   - Transition notifications
 *   - Runtime power statistics
 */

#include <ove/ove.hpp>
#include <cstdio>

#ifdef CONFIG_OVE_PM
namespace pm = ove::pm;
#endif

/* --- Simulated battery level --- */

static int battery_pct = 85;

/* --- PM transition notifier --- */

#ifdef CONFIG_OVE_PM
static void pm_notify(ove_pm_event_t event, ove_pm_state_t from,
		       ove_pm_state_t to, void *user_data)
{
	(void)user_data;

	if (event == OVE_PM_EVENT_PRE_SLEEP) {
		OVE_LOG_INF("pm: preparing sleep %d -> %d", from, to);
	} else {
		OVE_LOG_INF("pm: woke %d -> %d", from, to);
	}
}

/* --- Battery-aware power policy --- */

static ove_pm_state_t battery_policy(ove_pm_state_t current,
				     uint32_t idle_ms,
				     uint32_t next_timeout_ms,
				     void *user_data)
{
	int *batt = static_cast<int *>(user_data);

	(void)current;
	(void)next_timeout_ms;

	if (*batt < 15) {
		if (idle_ms > 5)
			return OVE_PM_STATE_DEEP_SLEEP;
		return OVE_PM_STATE_STANDBY;
	}

	if (idle_ms < 10) return OVE_PM_STATE_ACTIVE;
	if (idle_ms < 1000) return OVE_PM_STATE_IDLE;
	if (idle_ms < 10000) return OVE_PM_STATE_STANDBY;
	return OVE_PM_STATE_DEEP_SLEEP;
}
#endif /* CONFIG_OVE_PM */

/* --- Thread entry points --- */

static void sensor_thread(void *arg);
static void monitor_thread(void *arg);

static ove::Thread<4096> sensor_th(sensor_thread, nullptr,
				    OVE_PRIO_NORMAL, "sensor");
static ove::Thread<4096> monitor_th(monitor_thread, nullptr,
				     OVE_PRIO_LOW, "monitor");

/* --- Sensor thread: periodic read with domain management --- */

static void sensor_thread(void *)
{
	uint32_t reading = 0;

	OVE_LOG_INF("sensor: started");

	while (true) {
#ifdef CONFIG_OVE_PM
		pm::domain_request(OVE_PM_DOMAIN_SENSOR);
		pm::activity();
#endif

		ove::thread::sleep_ms(50);
		reading += 17;
		OVE_LOG_INF("sensor: reading = %u", reading % 1000);

#ifdef CONFIG_OVE_PM
		pm::domain_release(OVE_PM_DOMAIN_SENSOR);
#endif

		ove::thread::sleep_ms(5000);
	}
}

/* --- Monitor thread: print power statistics --- */

static void monitor_thread(void *)
{
	OVE_LOG_INF("monitor: started");

	while (true) {
		ove::thread::sleep_ms(10000);

#ifdef CONFIG_OVE_PM
		pm::Stats stats{};
		if (pm::get_stats(stats) == OVE_OK) {
			OVE_LOG_INF("=== Power Stats ===");
			OVE_LOG_INF("  active:  %u us (%u trans)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_ACTIVE],
				    stats.transition_count[OVE_PM_STATE_ACTIVE]);
			OVE_LOG_INF("  idle:    %u us (%u trans)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_IDLE],
				    stats.transition_count[OVE_PM_STATE_IDLE]);
			OVE_LOG_INF("  standby: %u us (%u trans)",
				    (unsigned)stats.time_in_state_us[OVE_PM_STATE_STANDBY],
				    stats.transition_count[OVE_PM_STATE_STANDBY]);
			OVE_LOG_INF("  active%%: %u.%02u%%",
				    stats.active_pct_x100 / 100,
				    stats.active_pct_x100 % 100);
		}
#endif

		if (battery_pct > 5)
			battery_pct -= 5;
		OVE_LOG_INF("battery: %d%%", battery_pct);
	}
}

/* --- App entry point --- */

OVE_MAIN()
{
	OVE_LOG_INF("pm example (C++): init");

#ifdef CONFIG_OVE_PM
	/* Initialize PM */
	pm::Cfg cfg{
		.idle_threshold_ms       = 50,
		.standby_threshold_ms    = 5000,
		.deep_sleep_threshold_ms = 30000,
	};
	int rc = pm::init(cfg);
	if (rc != OVE_OK) {
		OVE_LOG_ERR("PM init failed: %d", rc);
		return;
	}

	/* Register wake sources */
	pm::WakeSrc btn{};
	btn.type = OVE_PM_WAKE_GPIO;
	btn.gpio = { .port = 0, .pin = 13, .edge = OVE_GPIO_IRQ_FALLING };
	pm::wake_register(btn);

	pm::WakeSrc uart{};
	uart.type = OVE_PM_WAKE_UART;
	uart.uart = { .instance = 0 };
	pm::wake_register(uart);

	/* Register notifier and policy */
	pm::notify_register(pm_notify);
	pm::set_policy(battery_policy, &battery_pct);
	pm::set_budget(6000);
#endif

	OVE_LOG_INF("pm example (C++): ready (battery=%d%%)", battery_pct);

	ove::run();

#ifdef CONFIG_OVE_PM
	pm::deinit();
#endif
	OVE_LOG_INF("pm example (C++): shutdown");
}
