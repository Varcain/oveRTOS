/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PM

#include "ove/pm.h"
#include "ove/hal/hal_pm.h"
#include "ove/sync.h"
#include "ove/time.h"
#include "ove/log.h"
#include <string.h>

/* Helper: ove_time_get_us takes a pointer; this returns the value. */
static inline uint64_t pm_now_us(void)
{
	uint64_t t = 0;
	ove_time_get_us(&t);
	return t;
}

#ifndef CONFIG_OVE_PM_MAX_WAKE_SOURCES
#define CONFIG_OVE_PM_MAX_WAKE_SOURCES 8
#endif

#ifndef CONFIG_OVE_PM_MAX_NOTIFIERS
#define CONFIG_OVE_PM_MAX_NOTIFIERS 4
#endif

/* ── Internal state ─────────────────────────────────────────────────── */

struct pm_wake_entry {
	struct ove_pm_wake_src src;
	int registered;
};

struct pm_notifier_entry {
	ove_pm_notify_fn fn;
	void *user_data;
};

static struct {
	/* State machine */
	volatile ove_pm_state_t current_state;
	volatile int activity_flag;
	uint64_t last_activity_us;
	uint64_t state_entry_us;

	/* Configuration */
	struct ove_pm_cfg cfg;

	/* Wake sources */
	struct pm_wake_entry wake_table[CONFIG_OVE_PM_MAX_WAKE_SOURCES];

	/* Power domains */
	int domain_refcount[OVE_PM_DOMAIN_COUNT];

	/* Policy */
	ove_pm_policy_fn policy_fn;
	void *policy_user_data;

	/* Notifications */
	struct pm_notifier_entry notifiers[CONFIG_OVE_PM_MAX_NOTIFIERS];
	int notifier_count;

	/* Statistics */
	uint64_t time_in_state_us[OVE_PM_STATE_COUNT];
	uint32_t transition_count[OVE_PM_STATE_COUNT];

	/* Budget */
	uint32_t budget_target_x100;

	/* Thread safety */
	ove_mutex_storage_t mtx_storage;
	ove_mutex_t mtx;

	int initialized;
} pm_ctx;

/* ── Default policy ─────────────────────────────────────────────────── */

static ove_pm_state_t default_policy(ove_pm_state_t current, uint32_t idle_ms,
				     uint32_t next_timeout_ms, void *user_data)
{
	(void)current;
	(void)user_data;

	if (idle_ms < pm_ctx.cfg.idle_threshold_ms)
		return OVE_PM_STATE_ACTIVE;
	if (next_timeout_ms < pm_ctx.cfg.standby_threshold_ms)
		return OVE_PM_STATE_IDLE;
	if (idle_ms < pm_ctx.cfg.deep_sleep_threshold_ms)
		return OVE_PM_STATE_STANDBY;
	return OVE_PM_STATE_DEEP_SLEEP;
}

/* ── Wake source helpers ────────────────────────────────────────────── */

static int wake_src_match(const struct ove_pm_wake_src *a, const struct ove_pm_wake_src *b)
{
	if (a->type != b->type)
		return 0;
	switch (a->type) {
	case OVE_PM_WAKE_GPIO:
		return a->gpio.port == b->gpio.port && a->gpio.pin == b->gpio.pin;
	case OVE_PM_WAKE_TIMER:
		return a->timer.timeout_ms == b->timer.timeout_ms;
	case OVE_PM_WAKE_UART:
		return a->uart.instance == b->uart.instance;
	case OVE_PM_WAKE_RTC:
		return a->rtc.alarm_ms == b->rtc.alarm_ms;
	}
	return 0;
}

/* ── Notification helpers ───────────────────────────────────────────── */

static void fire_notifications(ove_pm_event_t event, ove_pm_state_t from, ove_pm_state_t to)
{
	int i;

	for (i = 0; i < pm_ctx.notifier_count; i++) {
		if (pm_ctx.notifiers[i].fn) {
			pm_ctx.notifiers[i].fn(event, from, to, pm_ctx.notifiers[i].user_data);
		}
	}
}

/* ── Statistics helpers ─────────────────────────────────────────────── */

static void update_stats(ove_pm_state_t old_state, uint64_t now_us)
{
	uint64_t elapsed = now_us - pm_ctx.state_entry_us;

	pm_ctx.time_in_state_us[old_state] += elapsed;
	pm_ctx.state_entry_us = now_us;
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

int ove_pm_init(const struct ove_pm_cfg *cfg)
{
	int rc;

	if (!cfg)
		return OVE_ERR_INVALID_PARAM;

	if (pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	memset(&pm_ctx, 0, sizeof(pm_ctx));
	pm_ctx.cfg = *cfg;
	pm_ctx.current_state = OVE_PM_STATE_ACTIVE;
	pm_ctx.policy_fn = default_policy;
	pm_ctx.last_activity_us = pm_now_us();
	pm_ctx.state_entry_us = pm_ctx.last_activity_us;

	rc = ove_mutex_init(&pm_ctx.mtx, &pm_ctx.mtx_storage);
	if (rc != OVE_OK)
		return rc;

	pm_ctx.initialized = 1;
	pm_ctx.transition_count[OVE_PM_STATE_ACTIVE] = 1;

	rc = ove_hal_pm_setup();
	if (rc != OVE_OK) {
		ove_mutex_deinit(pm_ctx.mtx);
		pm_ctx.initialized = 0;
		return rc;
	}

	OVE_LOG_INF("pm: initialized (idle=%u ms, standby=%u ms, deep=%u ms)",
		    cfg->idle_threshold_ms, cfg->standby_threshold_ms,
		    cfg->deep_sleep_threshold_ms);
	return OVE_OK;
}

void ove_pm_deinit(void)
{
	if (!pm_ctx.initialized)
		return;

	ove_hal_pm_teardown();
	ove_mutex_deinit(pm_ctx.mtx);
	pm_ctx.initialized = 0;

	OVE_LOG_INF("pm: deinitialized");
}

/* ── State machine ──────────────────────────────────────────────────── */

int ove_pm_set_state(ove_pm_state_t state)
{
	uint64_t now;
	ove_pm_state_t old;

	if (!pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;
	if (state >= OVE_PM_STATE_COUNT)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	old = pm_ctx.current_state;
	if (old == state) {
		ove_mutex_unlock(pm_ctx.mtx);
		return OVE_OK;
	}

	now = pm_now_us();
	update_stats(old, now);

	pm_ctx.current_state = state;
	pm_ctx.transition_count[state]++;

	/* Manual sleep entry anchors the idle baseline to now.  This
	 * preserves the user's explicit choice against the idle policy:
	 * ove_pm_idle_process() compares (now - last_activity_us) against
	 * the policy's escalation thresholds.  If we didn't reset here, a
	 * pre-existing long idle stretch would cause the next idle tick to
	 * immediately escalate past the user's selected state (e.g. user
	 * picks STANDBY, idle loop jumps to DEEP_SLEEP one tick later). */
	if (state > OVE_PM_STATE_ACTIVE) {
		pm_ctx.last_activity_us = now;
	}

	ove_mutex_unlock(pm_ctx.mtx);
	return OVE_OK;
}

ove_pm_state_t ove_pm_get_state(void)
{
	return pm_ctx.current_state;
}

void ove_pm_activity(void)
{
	pm_ctx.activity_flag = 1;
}

/* ── Wake sources ───────────────────────────────────────────────────── */

int ove_pm_wake_register(const struct ove_pm_wake_src *src)
{
	int i;

	if (!src || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	for (i = 0; i < CONFIG_OVE_PM_MAX_WAKE_SOURCES; i++) {
		if (!pm_ctx.wake_table[i].registered) {
			pm_ctx.wake_table[i].src = *src;
			pm_ctx.wake_table[i].registered = 1;
			ove_mutex_unlock(pm_ctx.mtx);
			return OVE_OK;
		}
	}

	ove_mutex_unlock(pm_ctx.mtx);
	return OVE_ERR_NO_MEMORY;
}

int ove_pm_wake_unregister(const struct ove_pm_wake_src *src)
{
	int i;

	if (!src || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	for (i = 0; i < CONFIG_OVE_PM_MAX_WAKE_SOURCES; i++) {
		if (pm_ctx.wake_table[i].registered &&
		    wake_src_match(&pm_ctx.wake_table[i].src, src)) {
			pm_ctx.wake_table[i].registered = 0;
			ove_mutex_unlock(pm_ctx.mtx);
			return OVE_OK;
		}
	}

	ove_mutex_unlock(pm_ctx.mtx);
	return OVE_ERR_NOT_REGISTERED;
}

/* ── Peripheral power domains ───────────────────────────────────────── */

int ove_pm_domain_request(ove_pm_domain_t domain)
{
	int rc = OVE_OK;

	if (domain >= OVE_PM_DOMAIN_COUNT || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	if (pm_ctx.domain_refcount[domain] == 0)
		rc = ove_hal_pm_domain_enable(domain);

	if (rc == OVE_OK)
		pm_ctx.domain_refcount[domain]++;

	ove_mutex_unlock(pm_ctx.mtx);
	return rc;
}

int ove_pm_domain_release(ove_pm_domain_t domain)
{
	int rc = OVE_OK;

	if (domain >= OVE_PM_DOMAIN_COUNT || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	if (pm_ctx.domain_refcount[domain] <= 0) {
		ove_mutex_unlock(pm_ctx.mtx);
		return OVE_ERR_INVALID_PARAM;
	}

	pm_ctx.domain_refcount[domain]--;

	if (pm_ctx.domain_refcount[domain] == 0)
		rc = ove_hal_pm_domain_disable(domain);

	ove_mutex_unlock(pm_ctx.mtx);
	return rc;
}

int ove_pm_domain_get_refcount(ove_pm_domain_t domain)
{
	if (domain >= OVE_PM_DOMAIN_COUNT || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	return pm_ctx.domain_refcount[domain];
}

/* ── Policy ─────────────────────────────────────────────────────────── */

int ove_pm_set_policy(ove_pm_policy_fn policy, void *user_data)
{
	if (!pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);
	pm_ctx.policy_fn = policy ? policy : default_policy;
	pm_ctx.policy_user_data = policy ? user_data : NULL;
	ove_mutex_unlock(pm_ctx.mtx);
	return OVE_OK;
}

/* ── Notifications ──────────────────────────────────────────────────── */

int ove_pm_notify_register(ove_pm_notify_fn cb, void *user_data)
{
	if (!cb || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	if (pm_ctx.notifier_count >= CONFIG_OVE_PM_MAX_NOTIFIERS) {
		ove_mutex_unlock(pm_ctx.mtx);
		return OVE_ERR_NO_MEMORY;
	}

	pm_ctx.notifiers[pm_ctx.notifier_count].fn = cb;
	pm_ctx.notifiers[pm_ctx.notifier_count].user_data = user_data;
	pm_ctx.notifier_count++;

	ove_mutex_unlock(pm_ctx.mtx);
	return OVE_OK;
}

int ove_pm_notify_unregister(ove_pm_notify_fn cb, void *user_data)
{
	int i;

	if (!cb || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	for (i = 0; i < pm_ctx.notifier_count; i++) {
		if (pm_ctx.notifiers[i].fn == cb && pm_ctx.notifiers[i].user_data == user_data) {
			/* Shift remaining entries down */
			int j;
			for (j = i; j < pm_ctx.notifier_count - 1; j++)
				pm_ctx.notifiers[j] = pm_ctx.notifiers[j + 1];
			pm_ctx.notifier_count--;
			pm_ctx.notifiers[pm_ctx.notifier_count].fn = NULL;
			pm_ctx.notifiers[pm_ctx.notifier_count].user_data = NULL;
			ove_mutex_unlock(pm_ctx.mtx);
			return OVE_OK;
		}
	}

	ove_mutex_unlock(pm_ctx.mtx);
	return OVE_ERR_NOT_REGISTERED;
}

/* ── Statistics ─────────────────────────────────────────────────────── */

int ove_pm_get_stats(struct ove_pm_stats *stats)
{
	uint64_t now;
	uint64_t total;
	int i;

	if (!stats || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	now = pm_now_us();

	/* Copy accumulated stats and add current state's ongoing time */
	memcpy(stats->time_in_state_us, pm_ctx.time_in_state_us, sizeof(stats->time_in_state_us));
	stats->time_in_state_us[pm_ctx.current_state] += now - pm_ctx.state_entry_us;

	memcpy(stats->transition_count, pm_ctx.transition_count, sizeof(stats->transition_count));

	total = 0;
	for (i = 0; i < OVE_PM_STATE_COUNT; i++)
		total += stats->time_in_state_us[i];

	stats->total_runtime_us = total;
	stats->active_pct_x100 =
		total > 0 ? (uint32_t)((stats->time_in_state_us[OVE_PM_STATE_ACTIVE] * 10000ULL) /
				       total)
			  : 10000;

	ove_mutex_unlock(pm_ctx.mtx);
	return OVE_OK;
}

void ove_pm_reset_stats(void)
{
	uint64_t now;

	if (!pm_ctx.initialized)
		return;

	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	now = pm_now_us();
	memset(pm_ctx.time_in_state_us, 0, sizeof(pm_ctx.time_in_state_us));
	memset(pm_ctx.transition_count, 0, sizeof(pm_ctx.transition_count));
	pm_ctx.state_entry_us = now;
	pm_ctx.transition_count[pm_ctx.current_state] = 1;

	ove_mutex_unlock(pm_ctx.mtx);
}

/* ── Power budget ───────────────────────────────────────────────────── */

int ove_pm_set_budget(uint32_t target_low_power_pct_x100)
{
	if (!pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	pm_ctx.budget_target_x100 = target_low_power_pct_x100;
	return OVE_OK;
}

int ove_pm_get_budget_status(uint32_t *actual_pct_x100)
{
	struct ove_pm_stats stats;
	int rc;

	if (!actual_pct_x100 || !pm_ctx.initialized)
		return OVE_ERR_INVALID_PARAM;

	rc = ove_pm_get_stats(&stats);
	if (rc != OVE_OK)
		return rc;

	/* Low-power % = 100% - active% */
	*actual_pct_x100 = 10000 - stats.active_pct_x100;
	return OVE_OK;
}

/* ── Idle processing (called from HAL idle hook) ────────────────────── */

void ove_pm_idle_process(void)
{
	uint64_t now;
	uint32_t idle_ms;
	uint32_t next_timeout_ms;
	ove_pm_state_t recommended;
	ove_pm_state_t old_state;
	int i;

	if (!pm_ctx.initialized)
		return;

	/* Check and clear activity flag (ISR-safe volatile read) */
	if (pm_ctx.activity_flag) {
		pm_ctx.activity_flag = 0;
		pm_ctx.last_activity_us = pm_now_us();
		if (pm_ctx.current_state != OVE_PM_STATE_ACTIVE) {
			ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);
			now = pm_now_us();
			update_stats(pm_ctx.current_state, now);
			pm_ctx.current_state = OVE_PM_STATE_ACTIVE;
			pm_ctx.transition_count[OVE_PM_STATE_ACTIVE]++;
			ove_mutex_unlock(pm_ctx.mtx);
		}
		return;
	}

	now = pm_now_us();
	idle_ms = (uint32_t)((now - pm_ctx.last_activity_us) / 1000ULL);
	next_timeout_ms = ove_hal_pm_get_next_timeout_ms();

	/* Consult policy */
	recommended = pm_ctx.policy_fn(pm_ctx.current_state, idle_ms, next_timeout_ms,
				       pm_ctx.policy_user_data);

	if (recommended >= OVE_PM_STATE_COUNT)
		recommended = OVE_PM_STATE_ACTIVE;

	if (recommended <= pm_ctx.current_state)
		return;

	/* Transition to deeper sleep */
	ove_mutex_lock(pm_ctx.mtx, OVE_WAIT_FOREVER);

	old_state = pm_ctx.current_state;
	now = pm_now_us();
	update_stats(old_state, now);

	/* Fire pre-sleep notifications */
	fire_notifications(OVE_PM_EVENT_PRE_SLEEP, old_state, recommended);

	/* Arm wake sources */
	for (i = 0; i < CONFIG_OVE_PM_MAX_WAKE_SOURCES; i++) {
		if (pm_ctx.wake_table[i].registered)
			ove_hal_pm_wake_arm(&pm_ctx.wake_table[i].src);
	}

	pm_ctx.current_state = recommended;
	pm_ctx.transition_count[recommended]++;

	/* Enter sleep — blocks until wake.
	 *
	 * On Cortex-M with plain WFI, this returns on the very next interrupt
	 * (typically the 1 kHz SysTick).  Keep update_stats authoritative for
	 * state_entry_us — DON'T touch last_activity_us here.  Treating wake
	 * as activity would reset the idle counter every tick and trap the
	 * system in a tight ACTIVE↔IDLE bounce, never escalating to STANDBY
	 * or DEEP_SLEEP.  Real activity comes from ove_pm_activity() (set by
	 * ISRs, application threads, registered wake sources). */
	ove_hal_pm_enter_state(recommended, next_timeout_ms);

	/* Woke up */
	now = pm_now_us();
	update_stats(recommended, now);
	pm_ctx.current_state = OVE_PM_STATE_ACTIVE;
	pm_ctx.transition_count[OVE_PM_STATE_ACTIVE]++;

	/* Disarm wake sources */
	for (i = 0; i < CONFIG_OVE_PM_MAX_WAKE_SOURCES; i++) {
		if (pm_ctx.wake_table[i].registered)
			ove_hal_pm_wake_disarm(&pm_ctx.wake_table[i].src);
	}

	/* Fire post-wake notifications */
	fire_notifications(OVE_PM_EVENT_POST_WAKE, recommended, OVE_PM_STATE_ACTIVE);

	ove_mutex_unlock(pm_ctx.mtx);
}

#endif /* CONFIG_OVE_PM */
