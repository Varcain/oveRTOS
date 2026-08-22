/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Qualification-only support for the Linux interop demo. Production flow in
 * app.c uses the same small lifecycle regardless of which probes a profile
 * enables; destructive tests, deadline measurement, and resource reporting
 * remain contained here.
 */

#include "qualification.h"

#include <stdio.h>

#include "ove/lxp_console.h"
#include "ove/time.h"
#if defined(CONFIG_OVE_WATCHDOG)
#include "ove/reset.h"
#include "ove/watchdog.h"
#endif

#include "ove_config.h"

#define THREAD_AUDIT_CAPACITY 2u

struct thread_audit_snapshot {
	const char *name;
	int available;
	size_t used;
	size_t size;
};

static struct thread_audit_snapshot g_thread_audits[THREAD_AUDIT_CAPACITY];
static size_t g_thread_audit_count;

#if defined(CONFIG_OVE_WATCHDOG)
/*
 * A high-priority task owns the IWDG feed. It feeds freely while the
 * coordinator is idle, but requires a changing coordinator heartbeat while a
 * guest is active. Guest progress is deliberately not part of this policy: an
 * isolated guest fault must not reset the host, and a guest cannot hold the
 * watchdog open.
 */
#define WD_TIMEOUT_MS 2000u
#define WD_FEED_MS 250u

static ove_thread_t g_wd;
#if defined(CONFIG_OVE_DEBUG_BUILD)
#define WD_STACK_SIZE 1024u
#else
#define WD_STACK_SIZE 512u
#endif
OVE_THREAD_DEFINE(g_wd_storage, WD_STACK_SIZE);
static ove_watchdog_t g_wd_dog;
static ove_watchdog_storage_t g_wd_dog_storage;
static int g_wd_started;

static int wd_should_feed(int active, int hb_advanced)
{
	return !active || hb_advanced;
}

#if defined(CONFIG_OVE_WATCHDOG_SELFTEST)
/*
 * Starve the coordinator without masking interrupts. The scheduler and monitor
 * stay alive, but the frozen heartbeat withholds the feed and proves the
 * watchdog policy end to end. A watchdog-recovery boot never re-trips it.
 */
static ove_thread_t g_wdtest;
OVE_THREAD_DEFINE(g_wdtest_storage, 512);

static void wdtest_spin(void *arg)
{
	(void)arg;
	for (;;)
		__asm volatile("nop");
}

static void wd_selftest_maybe_trip(void)
{
	if (ove_reset_cause() == OVE_RESET_WATCHDOG) {
		ove_lxp_console_write(
			"[wd] selftest: recovered from the watchdog reset; not re-tripping\n");
		return;
	}
	ove_lxp_console_write("[wd] selftest: starving the coordinator (scheduler stays live);"
			      " expect a watchdog reset in ~2s...\n");
	(void)ove_thread_init(&g_wdtest, &g_wdtest_storage, "wdtest", wdtest_spin, NULL,
			      OVE_PRIO_ABOVE_NORMAL, sizeof(g_wdtest_storage_stack),
			      g_wdtest_storage_stack);
}
#endif /* CONFIG_OVE_WATCHDOG_SELFTEST */

static void wd_body(void *arg)
{
	(void)arg;
	if (ove_watchdog_init(&g_wd_dog, &g_wd_dog_storage, WD_TIMEOUT_MS) != OVE_OK ||
	    ove_watchdog_start(g_wd_dog) != OVE_OK) {
		ove_lxp_console_write(
			"[wd] FAIL: could not arm IWDG; running without a watchdog\n");
		return;
	}
	ove_lxp_console_write(
		"[wd] IWDG armed: 2000ms timeout, fed every 250ms while the host stays live\n");

	uint32_t last = 0;
	int primed = 0;
#if defined(CONFIG_OVE_WATCHDOG_SELFTEST)
	unsigned cycle = 0;
	int tripped = 0;
#endif
	for (;;) {
		ove_lxp_run_health_t h;
		ove_lxp_run_health_snapshot(&h);
		int feed = !primed || wd_should_feed(h.active, h.coordinator_iterations != last);
		last = h.coordinator_iterations;
		primed = 1;
		if (feed)
			(void)ove_watchdog_feed(g_wd_dog);
#if defined(CONFIG_OVE_WATCHDOG_SELFTEST)
		if (!tripped && cycle >= 16 && h.active) {
			tripped = 1;
			wd_selftest_maybe_trip();
		}
		cycle++;
#endif
		ove_thread_sleep_ms(WD_FEED_MS);
	}
}
#endif /* CONFIG_OVE_WATCHDOG */

#if defined(CONFIG_OVE_LINUX_FAULTTEST)
/* Prove that a privileged host fault is fatal rather than guest-contained. */
static ove_thread_t g_ftest;
OVE_THREAD_DEFINE(g_ftest_storage, 512);

static void ftest_body(void *arg)
{
	(void)arg;
	ove_thread_sleep_ms(4000);
	ove_lxp_console_write("[c6] faulting a privileged host task (udf) while a guest runs;"
			      " expect HOST FAULT + watchdog reset\n");
	__asm volatile("udf #0");
	for (;;) {
	}
}

static void faulttest_maybe_arm(void)
{
	if (ove_reset_cause() == OVE_RESET_WATCHDOG) {
		ove_lxp_console_write("[c6] recovered from the host-fault test; not re-arming\n");
		return;
	}
	(void)ove_thread_init(&g_ftest, &g_ftest_storage, "ftest", ftest_body, NULL,
			      OVE_PRIO_NORMAL, sizeof(g_ftest_storage_stack),
			      g_ftest_storage_stack);
}
#endif /* CONFIG_OVE_LINUX_FAULTTEST */

#if defined(CONFIG_OVE_LINUX_SMASHTEST)
/* Prove that a host stack canary failure reaches the board's fatal handler. */
static ove_thread_t g_smash;
OVE_THREAD_DEFINE(g_smash_storage, 512);

static __attribute__((noinline)) void smash_host_stack(void)
{
	volatile char buf[16];
	volatile char *p = buf;
	for (int i = 0; i < 40; i++)
		p[i] = (char)(0xa5 + i);
	__asm__ volatile("" : : "r"(p) : "memory");
}

static void smashtest_body(void *arg)
{
	(void)arg;
	ove_thread_sleep_ms(4500);
	ove_lxp_console_write(
		"[c9] smashing a host stack buffer; expect STACK SMASH + watchdog reset\n");
	smash_host_stack();
	for (;;) {
	}
}

static void smashtest_maybe_arm(void)
{
	if (ove_reset_cause() == OVE_RESET_WATCHDOG) {
		ove_lxp_console_write("[c9] recovered from the smash test; not re-arming\n");
		return;
	}
	(void)ove_thread_init(&g_smash, &g_smash_storage, "smash", smashtest_body, NULL,
			      OVE_PRIO_NORMAL, sizeof(g_smash_storage_stack),
			      g_smash_storage_stack);
}
#endif /* CONFIG_OVE_LINUX_SMASHTEST */

#if defined(CONFIG_OVE_LINUX_LATENCY)
/* Measure high-priority host wake overshoot over exactly one guest run. */
#define MON_PERIOD_MS 10u

static ove_thread_t g_mon;
#if defined(CONFIG_OVE_DEBUG_BUILD)
#define MON_STACK_SIZE 1024u
#else
#define MON_STACK_SIZE 512u
#endif
OVE_THREAD_DEFINE(g_mon_storage, MON_STACK_SIZE);
static ove_lxp_latency_stat_t g_mon_late;
static int g_mon_started;

static void mon_body(void *arg)
{
	(void)arg;
	ove_thread_t self = ove_thread_get_self();
	const uint64_t period_ns = OVE_MS(MON_PERIOD_MS);
	while (!ove_thread_should_stop(self)) {
		uint64_t t0 = ove_time_now_steady_ns();
		ove_thread_sleep_ms(MON_PERIOD_MS);
		uint64_t slept = ove_time_now_steady_ns() - t0;
		ove_lxp_latency_record(&g_mon_late, slept > period_ns ? slept - period_ns : 0);
	}
}

static void lat_row(const char *what, const char *name, const ove_lxp_latency_stat_t *stat)
{
	if (!stat || !stat->count)
		return;
	ove_lxp_console_printf("[lat] %s%s%s n=%u max_ns=%u us[%u %u %u %u %u %u %u %u]\n", what,
			       name[0] ? " " : "", name, (unsigned int)stat->count,
			       (unsigned int)stat->max_ns, (unsigned int)stat->buckets[0],
			       (unsigned int)stat->buckets[1], (unsigned int)stat->buckets[2],
			       (unsigned int)stat->buckets[3], (unsigned int)stat->buckets[4],
			       (unsigned int)stat->buckets[5], (unsigned int)stat->buckets[6],
			       (unsigned int)stat->buckets[7]);
}

static void lat_report(const ove_lxp_host_observation_t *observation)
{
	ove_lxp_console_write("\n=== latency (measurement build; no threshold is enforced) ===\n"
			      "[lat] us[] buckets: <1 <2 <4 <8 <16 <32 <64 >=64\n");
	lat_row("host-wake-overshoot", "", &g_mon_late);
	for (uint32_t row = 0; row < observation->latency_service_count; row++)
		lat_row("coord-service", ove_lxp_observation_service_name(observation, row),
			&observation->latency_services[row].stat);
	for (uint32_t row = 0; row < observation->latency_wake_count; row++) {
		char name[12];
		(void)snprintf(name, sizeof(name), "%u",
			       (unsigned int)observation->latency_wakes[row].id);
		lat_row("guest-wake slot", name, &observation->latency_wakes[row].stat);
	}
	ove_lxp_console_write("[lat] end\n");
	ove_time_delay_ms(50);
}
#endif /* CONFIG_OVE_LINUX_LATENCY */

static void audit_stack_line(const char *name, int available, size_t used, size_t size)
{
	if (!available) {
		ove_lxp_console_printf("[stack] %-18s unavailable size=%u\n", name,
				       (unsigned int)size);
		return;
	}
	ove_lxp_console_printf("[stack] %-18s used=%u size=%u free=%u\n", name, (unsigned int)used,
			       (unsigned int)size, (unsigned int)(size > used ? size - used : 0));
}

static void audit_thread(const char *name, ove_thread_t thread, size_t stack_size)
{
	size_t headroom = 0;
	int available = ove_thread_get_stack_headroom(thread, &headroom) == OVE_OK;
	audit_stack_line(name, available, stack_size > headroom ? stack_size - headroom : 0,
			 stack_size);
}

static void host_observation_audit(const ove_lxp_host_observation_t *observation)
{
	const ove_lxp_size_observation_t *sizes = &observation->sizes;
	ove_lxp_console_printf("[lxp-size] slots=%u regions=%u proc=%u slot-core=%u region-core=%u "
			       "slot-table=%u coord-static=%u exec-capture=%u\n",
			       (unsigned int)sizes->slots, (unsigned int)sizes->regions,
			       (unsigned int)sizes->proc, (unsigned int)sizes->per_slot_core,
			       (unsigned int)sizes->per_region_core,
			       (unsigned int)sizes->slot_table,
			       (unsigned int)sizes->coordinator_static,
			       (unsigned int)sizes->exec_capture);
	ove_lxp_console_printf(
		"[lxp-size] mm=%u files=%u fs=%u sighand=%u group=%u arena=%u resume=%u "
		"mailbox=%u signal=%u\n",
		(unsigned int)sizes->mm, (unsigned int)sizes->files, (unsigned int)sizes->fs,
		(unsigned int)sizes->sighand, (unsigned int)sizes->thread_group,
		(unsigned int)sizes->arena, (unsigned int)sizes->resume_context,
		(unsigned int)sizes->deferred_request, (unsigned int)sizes->signal_save_stack);

	const ove_lxp_diagnostics_observation_t *health = &observation->diagnostics;
	if (!health->failures) {
		ove_lxp_console_printf("[lxp-world] checks=%u failures=0\n",
				       (unsigned int)health->checks);
	} else {
		ove_lxp_console_printf(
			"[lxp-world] checks=%u failures=%u first=%s slot=%d region=%d last=%s\n",
			(unsigned int)health->checks, (unsigned int)health->failures,
			ove_lxp_observation_issue_name(health->first_error.issue),
			(int)health->first_error.slot, (int)health->first_error.region,
			ove_lxp_observation_issue_name(health->last_error.issue));
	}
}

void linux_interop_qualification_start(void)
{
	g_thread_audit_count = 0;
#if defined(CONFIG_OVE_WATCHDOG)
	ove_lxp_console_write("[reset] cause: ");
	ove_lxp_console_write(ove_reset_cause_str(ove_reset_cause()));
	ove_lxp_console_write("\n");
	if (ove_thread_init(&g_wd, &g_wd_storage, "wd", wd_body, NULL, OVE_PRIO_HIGH,
			    sizeof(g_wd_storage_stack), g_wd_storage_stack) != OVE_OK) {
		ove_lxp_console_write(
			"[wd] FAIL: monitor thread init; running without a watchdog\n");
		return;
	}
	g_wd_started = 1;
#endif
}

void linux_interop_qualification_observe_thread(const char *name, ove_thread_t thread,
						size_t stack_size)
{
	if (!name || !thread || !stack_size || g_thread_audit_count >= THREAD_AUDIT_CAPACITY)
		return;

	size_t headroom = 0;
	int available = ove_thread_get_stack_headroom(thread, &headroom) == OVE_OK;
	g_thread_audits[g_thread_audit_count++] = (struct thread_audit_snapshot){
		.name = name,
		.available = available,
		.used = available && stack_size > headroom ? stack_size - headroom : 0,
		.size = stack_size,
	};
}

void linux_interop_qualification_arm_guest_tests(void)
{
#if defined(CONFIG_OVE_LINUX_FAULTTEST)
	faulttest_maybe_arm();
#endif
#if defined(CONFIG_OVE_LINUX_SMASHTEST)
	smashtest_maybe_arm();
#endif
}

int linux_interop_qualification_measurement_start(void)
{
#if defined(CONFIG_OVE_LINUX_LATENCY)
	g_mon_late = (ove_lxp_latency_stat_t){0};
	int rc = ove_thread_init(&g_mon, &g_mon_storage, "lat-mon", mon_body, NULL, OVE_PRIO_HIGH,
				 sizeof(g_mon_storage_stack), g_mon_storage_stack);
	if (rc != OVE_OK)
		return rc;
	g_mon_started = 1;
#endif
	return OVE_OK;
}

void linux_interop_qualification_measurement_stop(void)
{
#if defined(CONFIG_OVE_LINUX_LATENCY)
	if (!g_mon_started)
		return;
	ove_thread_request_stop(g_mon);
	while (ove_thread_get_state(g_mon) != OVE_THREAD_STATE_TERMINATED)
		ove_thread_sleep_ms(1);
	linux_interop_qualification_observe_thread("lat-monitor", g_mon,
						   sizeof(g_mon_storage_stack));
	(void)ove_thread_deinit(g_mon);
	g_mon_started = 0;
#endif
}

void linux_interop_qualification_report(const ove_lxp_host_observation_t *observation,
					ove_thread_t coordinator, size_t coordinator_stack_size)
{
#if defined(CONFIG_OVE_LINUX_LATENCY)
	lat_report(observation);
#endif
	host_observation_audit(observation);
	ove_lxp_console_write("\n=== stack high-water audit (deepest usage this run) ===\n");
	audit_thread("coordinator", coordinator, coordinator_stack_size);
	for (size_t i = 0; i < g_thread_audit_count; i++)
		audit_stack_line(g_thread_audits[i].name, g_thread_audits[i].available,
				 g_thread_audits[i].used, g_thread_audits[i].size);
#if defined(CONFIG_OVE_WATCHDOG)
	if (g_wd_started)
		audit_thread("wd-monitor", g_wd, sizeof(g_wd_storage_stack));
#endif
	if (observation->guest_stack.available)
		audit_stack_line("guest-slot(tramp)", 1, observation->guest_stack.used,
				 observation->guest_stack.size);

	struct ove_mem_stats memory;
	if (ove_sys_get_mem_stats(&memory) == OVE_OK) {
		ove_lxp_console_printf("[heap] free=%u peak_used=%u total=%u\n",
				       (unsigned int)memory.free, (unsigned int)memory.peak_used,
				       (unsigned int)memory.total);
	}
	ove_lxp_console_write("[stack] end\n");
	ove_time_delay_ms(50);
}
