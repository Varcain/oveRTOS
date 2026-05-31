/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static struct ove_pm_cfg default_cfg = {
	.idle_threshold_ms = 10,
	.standby_threshold_ms = 1000,
	.deep_sleep_threshold_ms = 10000,
};

/* Notification tracking */
static volatile int notify_pre_count;
static volatile int notify_post_count;
static volatile ove_pm_state_t notify_last_from;
static volatile ove_pm_state_t notify_last_to;
static volatile ove_pm_event_t notify_last_event;

static void test_notify_cb(ove_pm_event_t event, ove_pm_state_t from, ove_pm_state_t to,
			   void *user_data)
{
	(void)user_data;
	notify_last_event = event;
	notify_last_from = from;
	notify_last_to = to;
	if (event == OVE_PM_EVENT_PRE_SLEEP)
		notify_pre_count++;
	else
		notify_post_count++;
}

/* Second notifier for multi-notifier tests */
static volatile int notify2_count;

static void test_notify_cb2(ove_pm_event_t event, ove_pm_state_t from, ove_pm_state_t to,
			    void *user_data)
{
	(void)event;
	(void)from;
	(void)to;
	(void)user_data;
	notify2_count++;
}

/* Notifier that tracks user_data */
static volatile void *notify_user_data_received;

static void test_notify_cb_userdata(ove_pm_event_t event, ove_pm_state_t from, ove_pm_state_t to,
				    void *user_data)
{
	(void)event;
	(void)from;
	(void)to;
	notify_user_data_received = user_data;
}

/* Custom policy helpers */
static ove_pm_state_t fixed_state_for_policy;

static ove_pm_state_t fixed_policy(ove_pm_state_t current, uint32_t idle_ms,
				   uint32_t next_timeout_ms, void *user_data)
{
	(void)current;
	(void)idle_ms;
	(void)next_timeout_ms;
	(void)user_data;
	return fixed_state_for_policy;
}

/* Policy that uses user_data */
static ove_pm_state_t userdata_policy(ove_pm_state_t current, uint32_t idle_ms,
				      uint32_t next_timeout_ms, void *user_data)
{
	(void)current;
	(void)idle_ms;
	(void)next_timeout_ms;
	int *level = (int *)user_data;
	if (*level < 10)
		return OVE_PM_STATE_DEEP_SLEEP;
	return OVE_PM_STATE_ACTIVE;
}

/* Thread helper for concurrency tests */
static volatile int domain_thread_done;

static void domain_stress_entry(void *arg)
{
	int i;
	ove_pm_domain_t domain = (ove_pm_domain_t)(uintptr_t)arg;
	for (i = 0; i < 50; i++) {
		ove_pm_domain_request(domain);
		ove_pm_domain_release(domain);
	}
	TEST_FLAG_SET(domain_thread_done, 1);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  1. LIFECYCLE TESTS
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_init_deinit(void **state)
{
	(void)state;
	int rc = ove_pm_init(&default_cfg);
	assert_int_equal(rc, OVE_OK);
	ove_pm_deinit();
}

static void test_pm_init_null_cfg(void **state)
{
	(void)state;
	int rc = ove_pm_init(NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_pm_double_init(void **state)
{
	(void)state;
	int rc = ove_pm_init(&default_cfg);
	assert_int_equal(rc, OVE_OK);

	/* Second init should fail */
	rc = ove_pm_init(&default_cfg);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_pm_deinit();
}

static void test_pm_deinit_without_init(void **state)
{
	(void)state;
	/* Should not crash */
	ove_pm_deinit();
}

static void test_pm_reinit_after_deinit(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);
	ove_pm_deinit();

	/* Re-init should succeed */
	int rc = ove_pm_init(&default_cfg);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);
	ove_pm_deinit();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  2. STATE MACHINE TESTS
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_default_state(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);
	ove_pm_deinit();
}

static void test_pm_set_all_states(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	int rc = ove_pm_set_state(OVE_PM_STATE_IDLE);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_IDLE);

	rc = ove_pm_set_state(OVE_PM_STATE_STANDBY);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_STANDBY);

	rc = ove_pm_set_state(OVE_PM_STATE_DEEP_SLEEP);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_DEEP_SLEEP);

	rc = ove_pm_set_state(OVE_PM_STATE_ACTIVE);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);

	ove_pm_deinit();
}

static void test_pm_set_state_invalid(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	int rc = ove_pm_set_state(OVE_PM_STATE_COUNT);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	rc = ove_pm_set_state((ove_pm_state_t)99);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_pm_deinit();
}

static void test_pm_set_same_state_noop(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	/* Setting same state should succeed as no-op */
	int rc = ove_pm_set_state(OVE_PM_STATE_ACTIVE);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);

	ove_pm_deinit();
}

static void test_pm_activity_resets_to_active(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	ove_pm_set_state(OVE_PM_STATE_IDLE);
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_IDLE);

	ove_pm_activity();
	ove_pm_idle_process();
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);

	ove_pm_deinit();
}

static void test_pm_activity_from_deep_sleep(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	ove_pm_set_state(OVE_PM_STATE_DEEP_SLEEP);
	ove_pm_activity();
	ove_pm_idle_process();
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);

	ove_pm_deinit();
}

static void test_pm_multiple_activities(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	/* Multiple activity calls should be idempotent */
	ove_pm_activity();
	ove_pm_activity();
	ove_pm_activity();
	ove_pm_idle_process();
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);

	ove_pm_deinit();
}

static void test_pm_not_initialized_operations(void **state)
{
	(void)state;
	/* All operations should fail gracefully when not initialized */
	assert_int_equal(ove_pm_set_state(OVE_PM_STATE_IDLE), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_pm_domain_request(OVE_PM_DOMAIN_RADIO), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_pm_domain_release(OVE_PM_DOMAIN_RADIO), OVE_ERR_INVALID_PARAM);

	struct ove_pm_wake_src src = {
		.type = OVE_PM_WAKE_GPIO,
		.gpio = {.port = 0, .pin = 0, .edge = OVE_GPIO_IRQ_RISING},
	};
	assert_int_equal(ove_pm_wake_register(&src), OVE_ERR_INVALID_PARAM);

	struct ove_pm_stats stats;
	assert_int_equal(ove_pm_get_stats(&stats), OVE_ERR_INVALID_PARAM);

	assert_int_equal(ove_pm_set_policy(fixed_policy, NULL), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_pm_notify_register(test_notify_cb, NULL), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_pm_set_budget(5000), OVE_ERR_INVALID_PARAM);

	uint32_t actual;
	assert_int_equal(ove_pm_get_budget_status(&actual), OVE_ERR_INVALID_PARAM);

	/* Should not crash */
	ove_pm_activity();
	ove_pm_idle_process();
	ove_pm_reset_stats();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  3. WAKE SOURCE TESTS
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_wake_register_gpio(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	struct ove_pm_wake_src src = {
		.type = OVE_PM_WAKE_GPIO,
		.gpio = {.port = 0, .pin = 13, .edge = OVE_GPIO_IRQ_FALLING},
	};
	int rc = ove_pm_wake_register(&src);
	assert_int_equal(rc, OVE_OK);

	rc = ove_pm_wake_unregister(&src);
	assert_int_equal(rc, OVE_OK);

	ove_pm_deinit();
}

static void test_pm_wake_register_timer(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	struct ove_pm_wake_src src = {
		.type = OVE_PM_WAKE_TIMER,
		.timer = {.timeout_ms = 5000},
	};
	int rc = ove_pm_wake_register(&src);
	assert_int_equal(rc, OVE_OK);

	rc = ove_pm_wake_unregister(&src);
	assert_int_equal(rc, OVE_OK);

	ove_pm_deinit();
}

static void test_pm_wake_register_uart(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	struct ove_pm_wake_src src = {
		.type = OVE_PM_WAKE_UART,
		.uart = {.instance = 0},
	};
	int rc = ove_pm_wake_register(&src);
	assert_int_equal(rc, OVE_OK);

	rc = ove_pm_wake_unregister(&src);
	assert_int_equal(rc, OVE_OK);

	ove_pm_deinit();
}

static void test_pm_wake_register_rtc(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	struct ove_pm_wake_src src = {
		.type = OVE_PM_WAKE_RTC,
		.rtc = {.alarm_ms = 60000},
	};
	int rc = ove_pm_wake_register(&src);
	assert_int_equal(rc, OVE_OK);

	rc = ove_pm_wake_unregister(&src);
	assert_int_equal(rc, OVE_OK);

	ove_pm_deinit();
}

static void test_pm_wake_register_null(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	int rc = ove_pm_wake_register(NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	rc = ove_pm_wake_unregister(NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_pm_deinit();
}

static void test_pm_wake_unregister_not_found(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	struct ove_pm_wake_src src = {
		.type = OVE_PM_WAKE_UART,
		.uart = {.instance = 99},
	};
	int rc = ove_pm_wake_unregister(&src);
	assert_int_equal(rc, OVE_ERR_NOT_REGISTERED);

	ove_pm_deinit();
}

static void test_pm_wake_table_full(void **state)
{
	(void)state;
	int i;
	int rc;

	ove_pm_init(&default_cfg);

	for (i = 0; i < CONFIG_OVE_PM_MAX_WAKE_SOURCES; i++) {
		struct ove_pm_wake_src src = {
			.type = OVE_PM_WAKE_GPIO,
			.gpio = {.port = 0, .pin = (unsigned int)i, .edge = OVE_GPIO_IRQ_RISING},
		};
		rc = ove_pm_wake_register(&src);
		assert_int_equal(rc, OVE_OK);
	}

	/* Table full — next should fail */
	struct ove_pm_wake_src extra = {
		.type = OVE_PM_WAKE_TIMER,
		.timer = {.timeout_ms = 1000},
	};
	rc = ove_pm_wake_register(&extra);
	assert_int_equal(rc, OVE_ERR_NO_MEMORY);

	ove_pm_deinit();
}

static void test_pm_wake_reuse_slot(void **state)
{
	(void)state;
	int i;

	ove_pm_init(&default_cfg);

	/* Fill table */
	for (i = 0; i < CONFIG_OVE_PM_MAX_WAKE_SOURCES; i++) {
		struct ove_pm_wake_src src = {
			.type = OVE_PM_WAKE_GPIO,
			.gpio = {.port = 0, .pin = (unsigned int)i, .edge = OVE_GPIO_IRQ_RISING},
		};
		ove_pm_wake_register(&src);
	}

	/* Remove one */
	struct ove_pm_wake_src remove = {
		.type = OVE_PM_WAKE_GPIO,
		.gpio = {.port = 0, .pin = 3, .edge = OVE_GPIO_IRQ_RISING},
	};
	int rc = ove_pm_wake_unregister(&remove);
	assert_int_equal(rc, OVE_OK);

	/* Should be able to register one more now */
	struct ove_pm_wake_src replacement = {
		.type = OVE_PM_WAKE_TIMER,
		.timer = {.timeout_ms = 2000},
	};
	rc = ove_pm_wake_register(&replacement);
	assert_int_equal(rc, OVE_OK);

	ove_pm_deinit();
}

static void test_pm_wake_mixed_types(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	struct ove_pm_wake_src gpio_src = {
		.type = OVE_PM_WAKE_GPIO,
		.gpio = {.port = 1, .pin = 5, .edge = OVE_GPIO_IRQ_BOTH},
	};
	struct ove_pm_wake_src timer_src = {
		.type = OVE_PM_WAKE_TIMER,
		.timer = {.timeout_ms = 3000},
	};
	struct ove_pm_wake_src uart_src = {
		.type = OVE_PM_WAKE_UART,
		.uart = {.instance = 1},
	};

	assert_int_equal(ove_pm_wake_register(&gpio_src), OVE_OK);
	assert_int_equal(ove_pm_wake_register(&timer_src), OVE_OK);
	assert_int_equal(ove_pm_wake_register(&uart_src), OVE_OK);

	/* Unregister in different order */
	assert_int_equal(ove_pm_wake_unregister(&timer_src), OVE_OK);
	assert_int_equal(ove_pm_wake_unregister(&gpio_src), OVE_OK);
	assert_int_equal(ove_pm_wake_unregister(&uart_src), OVE_OK);

	ove_pm_deinit();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  4. PERIPHERAL POWER DOMAIN TESTS
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_domain_request_release(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	int rc = ove_pm_domain_request(OVE_PM_DOMAIN_RADIO);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_RADIO), 1);

	rc = ove_pm_domain_release(OVE_PM_DOMAIN_RADIO);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_RADIO), 0);

	ove_pm_deinit();
}

static void test_pm_domain_underflow(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);
	int rc = ove_pm_domain_release(OVE_PM_DOMAIN_SENSOR);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	ove_pm_deinit();
}

static void test_pm_domain_multiple_users(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	ove_pm_domain_request(OVE_PM_DOMAIN_DISPLAY);
	ove_pm_domain_request(OVE_PM_DOMAIN_DISPLAY);
	ove_pm_domain_request(OVE_PM_DOMAIN_DISPLAY);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_DISPLAY), 3);

	ove_pm_domain_release(OVE_PM_DOMAIN_DISPLAY);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_DISPLAY), 2);

	ove_pm_domain_release(OVE_PM_DOMAIN_DISPLAY);
	ove_pm_domain_release(OVE_PM_DOMAIN_DISPLAY);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_DISPLAY), 0);

	ove_pm_deinit();
}

static void test_pm_domain_invalid(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	assert_int_equal(ove_pm_domain_request(OVE_PM_DOMAIN_COUNT), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_pm_domain_release(OVE_PM_DOMAIN_COUNT), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_COUNT), OVE_ERR_INVALID_PARAM);

	ove_pm_deinit();
}

static void test_pm_domain_all_domains(void **state)
{
	(void)state;
	int i;
	ove_pm_init(&default_cfg);

	/* Request all domains */
	for (i = 0; i < OVE_PM_DOMAIN_COUNT; i++) {
		int rc = ove_pm_domain_request((ove_pm_domain_t)i);
		assert_int_equal(rc, OVE_OK);
		assert_int_equal(ove_pm_domain_get_refcount((ove_pm_domain_t)i), 1);
	}

	/* Release all domains */
	for (i = 0; i < OVE_PM_DOMAIN_COUNT; i++) {
		int rc = ove_pm_domain_release((ove_pm_domain_t)i);
		assert_int_equal(rc, OVE_OK);
		assert_int_equal(ove_pm_domain_get_refcount((ove_pm_domain_t)i), 0);
	}

	ove_pm_deinit();
}

static void test_pm_domain_independent(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	ove_pm_domain_request(OVE_PM_DOMAIN_RADIO);
	ove_pm_domain_request(OVE_PM_DOMAIN_SENSOR);

	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_RADIO), 1);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_SENSOR), 1);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_DISPLAY), 0);

	ove_pm_domain_release(OVE_PM_DOMAIN_RADIO);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_RADIO), 0);
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_SENSOR), 1);

	ove_pm_domain_release(OVE_PM_DOMAIN_SENSOR);
	ove_pm_deinit();
}

static void test_pm_domain_concurrent(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	domain_thread_done = 0;
	OVE_TEST_STORAGE(ove_thread_storage_t, t_stor);
	OVE_TEST_STACK(t_stack, 2048);
	ove_thread_t th;

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_init(&th, &t_stor, "dom_stress", domain_stress_entry,
			(void *)(uintptr_t)OVE_PM_DOMAIN_AUDIO, OVE_PRIO_NORMAL, 2048, t_stack);
#else
	(void)t_stor;
	(void)t_stack;
	ove_thread_create(&th, "dom_stress", domain_stress_entry,
			  (void *)(uintptr_t)OVE_PM_DOMAIN_AUDIO, OVE_PRIO_NORMAL, 2048);
#endif

	/* Main thread also hammers same domain */
	int i;
	for (i = 0; i < 50; i++) {
		ove_pm_domain_request(OVE_PM_DOMAIN_AUDIO);
		ove_pm_domain_release(OVE_PM_DOMAIN_AUDIO);
	}

	/* Wait for other thread */
	(void)wait_for_flag(&domain_thread_done, 1, 5000);
	test_msleep(10);

	/* Refcount should be zero after both threads balanced */
	assert_int_equal(ove_pm_domain_get_refcount(OVE_PM_DOMAIN_AUDIO), 0);

	/* Join + free the worker before PM teardown.  ove_test_thread_destroy
	 * blocks until the entry returns, so this also joins — without it the
	 * thread handle/storage leaks every run (all other thread tests pair
	 * create with destroy). */
	ove_test_thread_destroy(th);

	ove_pm_deinit();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  5. POLICY TESTS
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_custom_policy(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	fixed_state_for_policy = OVE_PM_STATE_STANDBY;
	int rc = ove_pm_set_policy(fixed_policy, NULL);
	assert_int_equal(rc, OVE_OK);

	ove_pm_deinit();
}

static void test_pm_restore_default_policy(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	ove_pm_set_policy(fixed_policy, NULL);

	/* NULL restores default */
	int rc = ove_pm_set_policy(NULL, NULL);
	assert_int_equal(rc, OVE_OK);

	ove_pm_deinit();
}

static void test_pm_policy_with_userdata(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	int battery_level = 5;
	int rc = ove_pm_set_policy(userdata_policy, &battery_level);
	assert_int_equal(rc, OVE_OK);

	/* Restore default for cleanup */
	ove_pm_set_policy(NULL, NULL);

	ove_pm_deinit();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  6. NOTIFICATION TESTS
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_notify_register_unregister(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	notify_pre_count = 0;
	notify_post_count = 0;

	int rc = ove_pm_notify_register(test_notify_cb, NULL);
	assert_int_equal(rc, OVE_OK);

	rc = ove_pm_notify_unregister(test_notify_cb, NULL);
	assert_int_equal(rc, OVE_OK);

	/* Double unregister should fail */
	rc = ove_pm_notify_unregister(test_notify_cb, NULL);
	assert_int_equal(rc, OVE_ERR_NOT_REGISTERED);

	ove_pm_deinit();
}

static void test_pm_notify_null_callback(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	int rc = ove_pm_notify_register(NULL, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);

	ove_pm_deinit();
}

static void test_pm_notify_table_full(void **state)
{
	(void)state;
	int i;

	ove_pm_init(&default_cfg);

	/* Fill notifier table (use different user_data to distinguish) */
	for (i = 0; i < CONFIG_OVE_PM_MAX_NOTIFIERS; i++) {
		int rc = ove_pm_notify_register(test_notify_cb, (void *)(uintptr_t)(i + 1));
		assert_int_equal(rc, OVE_OK);
	}

	/* Next should fail */
	int rc = ove_pm_notify_register(test_notify_cb2, NULL);
	assert_int_equal(rc, OVE_ERR_NO_MEMORY);

	/* Cleanup */
	for (i = 0; i < CONFIG_OVE_PM_MAX_NOTIFIERS; i++) {
		ove_pm_notify_unregister(test_notify_cb, (void *)(uintptr_t)(i + 1));
	}

	ove_pm_deinit();
}

static void test_pm_notify_multiple_notifiers(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	notify_pre_count = 0;
	notify2_count = 0;

	ove_pm_notify_register(test_notify_cb, NULL);
	ove_pm_notify_register(test_notify_cb2, NULL);

	/* Trigger a transition via idle_process with a custom policy */
	fixed_state_for_policy = OVE_PM_STATE_IDLE;
	ove_pm_set_policy(fixed_policy, NULL);
	ove_pm_idle_process();

	/* Both notifiers should have fired */
	assert_true(notify_pre_count > 0 || notify_post_count > 0);
	assert_true(notify2_count > 0);

	ove_pm_set_policy(NULL, NULL);
	ove_pm_notify_unregister(test_notify_cb, NULL);
	ove_pm_notify_unregister(test_notify_cb2, NULL);

	ove_pm_deinit();
}

static void test_pm_notify_userdata_forwarded(void **state)
{
	(void)state;
	int magic = 42;

	ove_pm_init(&default_cfg);
	notify_user_data_received = NULL;

	ove_pm_notify_register(test_notify_cb_userdata, &magic);

	/* Force a transition to trigger the notifier */
	fixed_state_for_policy = OVE_PM_STATE_IDLE;
	ove_pm_set_policy(fixed_policy, NULL);
	ove_pm_idle_process();

	assert_ptr_equal(notify_user_data_received, &magic);

	ove_pm_set_policy(NULL, NULL);
	ove_pm_notify_unregister(test_notify_cb_userdata, &magic);
	ove_pm_deinit();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  7. STATISTICS TESTS
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_stats_initial(void **state)
{
	(void)state;
	struct ove_pm_stats stats;

	ove_pm_init(&default_cfg);
	test_msleep(1); /* ensure measurable elapsed time */
	int rc = ove_pm_get_stats(&stats);
	assert_int_equal(rc, OVE_OK);

	/* Initial state is ACTIVE with 1 transition */
	assert_int_equal(stats.transition_count[OVE_PM_STATE_ACTIVE], 1);
	assert_int_equal(stats.transition_count[OVE_PM_STATE_IDLE], 0);
	assert_int_equal(stats.transition_count[OVE_PM_STATE_STANDBY], 0);
	assert_int_equal(stats.transition_count[OVE_PM_STATE_DEEP_SLEEP], 0);
	assert_true(stats.total_runtime_us > 0);

	ove_pm_deinit();
}

static void test_pm_stats_null_param(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);
	int rc = ove_pm_get_stats(NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	ove_pm_deinit();
}

static void test_pm_stats_tracking(void **state)
{
	(void)state;
	struct ove_pm_stats stats;

	ove_pm_init(&default_cfg);

	ove_pm_set_state(OVE_PM_STATE_IDLE);
	test_msleep(10);
	ove_pm_set_state(OVE_PM_STATE_STANDBY);
	test_msleep(10);
	ove_pm_set_state(OVE_PM_STATE_ACTIVE);
	test_msleep(1); /* ensure measurable active time for active_pct */

	int rc = ove_pm_get_stats(&stats);
	assert_int_equal(rc, OVE_OK);

	/* Should have transitions for ACTIVE→IDLE→STANDBY→ACTIVE */
	assert_true(stats.transition_count[OVE_PM_STATE_ACTIVE] >= 2);
	assert_true(stats.transition_count[OVE_PM_STATE_IDLE] >= 1);
	assert_true(stats.transition_count[OVE_PM_STATE_STANDBY] >= 1);

	/* Should have recorded some time in IDLE and STANDBY */
	assert_true(stats.time_in_state_us[OVE_PM_STATE_IDLE] > 0);
	assert_true(stats.time_in_state_us[OVE_PM_STATE_STANDBY] > 0);
	assert_true(stats.total_runtime_us > 0);

	/* active_pct should be reasonable (not 0 and not 100%) */
	assert_true(stats.active_pct_x100 > 0);
	assert_true(stats.active_pct_x100 <= 10000);

	ove_pm_deinit();
}

static void test_pm_stats_reset(void **state)
{
	(void)state;
	struct ove_pm_stats stats;

	ove_pm_init(&default_cfg);
	ove_pm_set_state(OVE_PM_STATE_IDLE);
	ove_pm_set_state(OVE_PM_STATE_STANDBY);
	ove_pm_set_state(OVE_PM_STATE_ACTIVE);

	ove_pm_reset_stats();
	ove_pm_get_stats(&stats);

	/* After reset: only ACTIVE entry, no other transitions */
	assert_int_equal(stats.transition_count[OVE_PM_STATE_ACTIVE], 1);
	assert_int_equal(stats.transition_count[OVE_PM_STATE_IDLE], 0);
	assert_int_equal(stats.transition_count[OVE_PM_STATE_STANDBY], 0);
	assert_int_equal(stats.transition_count[OVE_PM_STATE_DEEP_SLEEP], 0);

	ove_pm_deinit();
}

static void test_pm_stats_active_pct_all_active(void **state)
{
	(void)state;
	struct ove_pm_stats stats;

	ove_pm_init(&default_cfg);
	test_msleep(10);

	ove_pm_get_stats(&stats);

	/* Should be ~100% active */
	assert_int_equal(stats.active_pct_x100, 10000);

	ove_pm_deinit();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  8. BUDGET TESTS
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_budget_set_get(void **state)
{
	(void)state;
	uint32_t actual;

	ove_pm_init(&default_cfg);

	int rc = ove_pm_set_budget(5000);
	assert_int_equal(rc, OVE_OK);

	rc = ove_pm_get_budget_status(&actual);
	assert_int_equal(rc, OVE_OK);
	assert_true(actual <= 10000);

	ove_pm_deinit();
}

static void test_pm_budget_null_param(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);
	int rc = ove_pm_get_budget_status(NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	ove_pm_deinit();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  9. IDLE PROCESSING INTEGRATION
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_pm_idle_process_no_transition_when_active(void **state)
{
	(void)state;
	ove_pm_init(&default_cfg);

	/* With activity just reported, idle_process should keep ACTIVE */
	ove_pm_activity();
	ove_pm_idle_process();
	assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);

	ove_pm_deinit();
}

static void test_pm_rapid_init_deinit_cycles(void **state)
{
	(void)state;
	int i;

	for (i = 0; i < 10; i++) {
		int rc = ove_pm_init(&default_cfg);
		assert_int_equal(rc, OVE_OK);
		assert_int_equal(ove_pm_get_state(), OVE_PM_STATE_ACTIVE);

		ove_pm_domain_request(OVE_PM_DOMAIN_RADIO);
		ove_pm_domain_release(OVE_PM_DOMAIN_RADIO);

		ove_pm_deinit();
	}
}

/* ── setup/teardown ──────────────────────────────────────────────────── */

static int pm_setup(void **state)
{
	(void)state;
	notify_pre_count = 0;
	notify_post_count = 0;
	notify_last_from = OVE_PM_STATE_ACTIVE;
	notify_last_to = OVE_PM_STATE_ACTIVE;
	notify_last_event = OVE_PM_EVENT_PRE_SLEEP;
	notify2_count = 0;
	notify_user_data_received = NULL;
	domain_thread_done = 0;
	return 0;
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_pm_run(void)
{
	const struct CMUnitTest tests[] = {
		/* 1. Lifecycle */
		cmocka_unit_test_setup(test_pm_init_deinit, pm_setup),
		cmocka_unit_test_setup(test_pm_init_null_cfg, pm_setup),
		cmocka_unit_test_setup(test_pm_double_init, pm_setup),
		cmocka_unit_test_setup(test_pm_deinit_without_init, pm_setup),
		cmocka_unit_test_setup(test_pm_reinit_after_deinit, pm_setup),

		/* 2. State machine */
		cmocka_unit_test_setup(test_pm_default_state, pm_setup),
		cmocka_unit_test_setup(test_pm_set_all_states, pm_setup),
		cmocka_unit_test_setup(test_pm_set_state_invalid, pm_setup),
		cmocka_unit_test_setup(test_pm_set_same_state_noop, pm_setup),
		cmocka_unit_test_setup(test_pm_activity_resets_to_active, pm_setup),
		cmocka_unit_test_setup(test_pm_activity_from_deep_sleep, pm_setup),
		cmocka_unit_test_setup(test_pm_multiple_activities, pm_setup),
		cmocka_unit_test_setup(test_pm_not_initialized_operations, pm_setup),

		/* 3. Wake sources */
		cmocka_unit_test_setup(test_pm_wake_register_gpio, pm_setup),
		cmocka_unit_test_setup(test_pm_wake_register_timer, pm_setup),
		cmocka_unit_test_setup(test_pm_wake_register_uart, pm_setup),
		cmocka_unit_test_setup(test_pm_wake_register_rtc, pm_setup),
		cmocka_unit_test_setup(test_pm_wake_register_null, pm_setup),
		cmocka_unit_test_setup(test_pm_wake_unregister_not_found, pm_setup),
		cmocka_unit_test_setup(test_pm_wake_table_full, pm_setup),
		cmocka_unit_test_setup(test_pm_wake_reuse_slot, pm_setup),
		cmocka_unit_test_setup(test_pm_wake_mixed_types, pm_setup),

		/* 4. Peripheral power domains */
		cmocka_unit_test_setup(test_pm_domain_request_release, pm_setup),
		cmocka_unit_test_setup(test_pm_domain_underflow, pm_setup),
		cmocka_unit_test_setup(test_pm_domain_multiple_users, pm_setup),
		cmocka_unit_test_setup(test_pm_domain_invalid, pm_setup),
		cmocka_unit_test_setup(test_pm_domain_all_domains, pm_setup),
		cmocka_unit_test_setup(test_pm_domain_independent, pm_setup),
		cmocka_unit_test_setup(test_pm_domain_concurrent, pm_setup),

		/* 5. Policy */
		cmocka_unit_test_setup(test_pm_custom_policy, pm_setup),
		cmocka_unit_test_setup(test_pm_restore_default_policy, pm_setup),
		cmocka_unit_test_setup(test_pm_policy_with_userdata, pm_setup),

		/* 6. Notifications */
		cmocka_unit_test_setup(test_pm_notify_register_unregister, pm_setup),
		cmocka_unit_test_setup(test_pm_notify_null_callback, pm_setup),
		cmocka_unit_test_setup(test_pm_notify_table_full, pm_setup),
		cmocka_unit_test_setup(test_pm_notify_multiple_notifiers, pm_setup),
		cmocka_unit_test_setup(test_pm_notify_userdata_forwarded, pm_setup),

		/* 7. Statistics */
		cmocka_unit_test_setup(test_pm_stats_initial, pm_setup),
		cmocka_unit_test_setup(test_pm_stats_null_param, pm_setup),
		cmocka_unit_test_setup(test_pm_stats_tracking, pm_setup),
		cmocka_unit_test_setup(test_pm_stats_reset, pm_setup),
		cmocka_unit_test_setup(test_pm_stats_active_pct_all_active, pm_setup),

		/* 8. Budget */
		cmocka_unit_test_setup(test_pm_budget_set_get, pm_setup),
		cmocka_unit_test_setup(test_pm_budget_null_param, pm_setup),

		/* 9. Integration */
		cmocka_unit_test_setup(test_pm_idle_process_no_transition_when_active, pm_setup),
		cmocka_unit_test_setup(test_pm_rapid_init_deinit_cycles, pm_setup),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
