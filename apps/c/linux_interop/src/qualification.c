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

#include "ove/lxp_console.h"
#include "ove/time.h"
#if defined(CONFIG_OVE_WATCHDOG)
#include "ove/reset.h"
#include "ove/watchdog.h"
#endif

#include "ove_config.h"

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/*
 * Small local formatters keep qualification independent of libc printf.
 * Keep them out of line: NuttX app sources use -O3, which otherwise duplicates
 * these loops at every diagnostics field and adds roughly 17 KiB of text.
 */
static __attribute__((noinline)) char *put_str(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static __attribute__((noinline)) char *put_dec(char *p, uint32_t v)
{
	char tmp[10];
	int i = 0;
	if (v == 0) {
		*p++ = '0';
		return p;
	}
	while (v) {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i)
		*p++ = tmp[--i];
	return p;
}

static __attribute__((noinline)) char *put_sdec(char *p, int32_t v)
{
	if (v < 0) {
		*p++ = '-';
		return put_dec(p, (uint32_t)(-(int64_t)v));
	}
	return put_dec(p, (uint32_t)v);
}

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
	UNUSED(arg);
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
	UNUSED(arg);
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
	UNUSED(arg);
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
	UNUSED(arg);
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
static volatile int g_mon_stop;
static volatile int g_mon_exited;
static int g_mon_started;

static void mon_body(void *arg)
{
	UNUSED(arg);
	while (!g_mon_stop) {
		uint64_t t0 = 0, t1 = 0;
		(void)ove_time_get_ns(&t0);
		ove_thread_sleep_ms(MON_PERIOD_MS);
		(void)ove_time_get_ns(&t1);
		uint64_t want = (uint64_t)MON_PERIOD_MS * 1000000u;
		uint64_t slept = (t1 > t0) ? (t1 - t0) : 0;
		ove_lxp_latency_record(&g_mon_late, slept > want ? slept - want : 0);
	}
	g_mon_exited = 1;
}

static void lat_row(const char *what, const char *name, const ove_lxp_latency_stat_t *stat)
{
	if (!stat || !stat->count)
		return;
	char line[192];
	char *p = put_str(line, "[lat] ");
	p = put_str(p, what);
	*p++ = ' ';
	p = put_str(p, name);
	while ((p - line) < 30)
		*p++ = ' ';
	p = put_str(p, "n=");
	p = put_dec(p, stat->count);
	p = put_str(p, " max_ns=");
	p = put_dec(p, stat->max_ns);
	p = put_str(p, " us[");
	for (int bucket = 0; bucket < OVE_LXP_LATENCY_BUCKETS; bucket++) {
		if (bucket)
			*p++ = ' ';
		p = put_dec(p, stat->buckets[bucket]);
	}
	p = put_str(p, "]\n");
	*p = 0;
	ove_lxp_console_write(line);
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
		char name[8];
		char *p = put_dec(name, observation->latency_wakes[row].id);
		*p = 0;
		lat_row("guest-wake slot", name, &observation->latency_wakes[row].stat);
	}
	ove_lxp_console_write("[lat] end\n");
	ove_time_delay_ms(50);
}
#endif /* CONFIG_OVE_LINUX_LATENCY */

static void audit_stack_line(const char *name, size_t used, size_t size)
{
	char line[96];
	char *p = put_str(line, "[stack] ");
	p = put_str(p, name);
	while ((p - line) < 26)
		*p++ = ' ';
	p = put_str(p, "used=");
	p = put_dec(p, (uint32_t)used);
	p = put_str(p, " size=");
	p = put_dec(p, (uint32_t)size);
	p = put_str(p, " free=");
	p = put_dec(p, (uint32_t)(size > used ? size - used : 0));
	*p++ = '\n';
	*p = 0;
	ove_lxp_console_write(line);
}

static void audit_thread(const linux_interop_thread_audit_t *thread)
{
	size_t free_bytes = ove_thread_get_stack_usage(thread->thread);
	audit_stack_line(thread->name,
			 thread->stack_size > free_bytes ? thread->stack_size - free_bytes : 0,
			 thread->stack_size);
}

static void host_observation_audit(const ove_lxp_host_observation_t *observation)
{
	const ove_lxp_size_observation_t *sizes = &observation->sizes;
	{
		char line[224];
		char *p = put_str(line, "[lxp-size] slots=");
		p = put_dec(p, sizes->slots);
		p = put_str(p, " regions=");
		p = put_dec(p, sizes->regions);
		p = put_str(p, " proc=");
		p = put_dec(p, (uint32_t)sizes->proc);
		p = put_str(p, " slot-core=");
		p = put_dec(p, (uint32_t)sizes->per_slot_core);
		p = put_str(p, " region-core=");
		p = put_dec(p, (uint32_t)sizes->per_region_core);
		p = put_str(p, " slot-table=");
		p = put_dec(p, (uint32_t)sizes->slot_table);
		p = put_str(p, " coord-static=");
		p = put_dec(p, (uint32_t)sizes->coordinator_static);
		p = put_str(p, " exec-capture=");
		p = put_dec(p, (uint32_t)sizes->exec_capture);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(line);
	}
	{
		char line[224];
		char *p = put_str(line, "[lxp-size] mm=");
		p = put_dec(p, (uint32_t)sizes->mm);
		p = put_str(p, " files=");
		p = put_dec(p, (uint32_t)sizes->files);
		p = put_str(p, " fs=");
		p = put_dec(p, (uint32_t)sizes->fs);
		p = put_str(p, " sighand=");
		p = put_dec(p, (uint32_t)sizes->sighand);
		p = put_str(p, " group=");
		p = put_dec(p, (uint32_t)sizes->thread_group);
		p = put_str(p, " arena=");
		p = put_dec(p, (uint32_t)sizes->arena);
		p = put_str(p, " resume=");
		p = put_dec(p, (uint32_t)sizes->resume_context);
		p = put_str(p, " mailbox=");
		p = put_dec(p, (uint32_t)sizes->deferred_request);
		p = put_str(p, " signal=");
		p = put_dec(p, (uint32_t)sizes->signal_save_stack);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(line);
	}

	const ove_lxp_diagnostics_observation_t *health = &observation->diagnostics;
	{
		char line[224];
		char *p = put_str(line, "[lxp-world] checks=");
		p = put_dec(p, health->checks);
		p = put_str(p, " failures=");
		p = put_dec(p, health->failures);
		if (health->failures) {
			p = put_str(p, " first=");
			p = put_str(p, ove_lxp_observation_issue_name(health->first_error.issue));
			p = put_str(p, " slot=");
			p = put_sdec(p, health->first_error.slot);
			p = put_str(p, " region=");
			p = put_sdec(p, health->first_error.region);
			p = put_str(p, " last=");
			p = put_str(p, ove_lxp_observation_issue_name(health->last_error.issue));
		}
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(line);
	}
}

void linux_interop_qualification_start(void)
{
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
	g_mon_stop = 0;
	g_mon_exited = 0;
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
	g_mon_stop = 1;
	while (!g_mon_exited)
		ove_thread_sleep_ms(1);
	(void)ove_thread_deinit(g_mon);
	g_mon_started = 0;
#endif
}

void linux_interop_qualification_report(const ove_lxp_host_observation_t *observation,
					const linux_interop_thread_audit_t *threads,
					size_t thread_count)
{
#if defined(CONFIG_OVE_LINUX_LATENCY)
	lat_report(observation);
#endif
	host_observation_audit(observation);
	ove_lxp_console_write("\n=== stack high-water audit (deepest usage this run) ===\n");
	for (size_t i = 0; i < thread_count; i++)
		audit_thread(&threads[i]);
#if defined(CONFIG_OVE_WATCHDOG)
	if (g_wd_started) {
		const linux_interop_thread_audit_t watchdog = {
			.name = "wd-monitor",
			.thread = g_wd,
			.stack_size = sizeof(g_wd_storage_stack),
		};
		audit_thread(&watchdog);
	}
#endif
#if defined(CONFIG_OVE_LINUX_LATENCY)
	{
		const linux_interop_thread_audit_t monitor = {
			.name = "lat-monitor",
			.thread = g_mon,
			.stack_size = sizeof(g_mon_storage_stack),
		};
		audit_thread(&monitor);
	}
#endif
	if (observation->guest_stack.available)
		audit_stack_line("guest-slot(tramp)", observation->guest_stack.used,
				 observation->guest_stack.size);

	struct ove_mem_stats memory;
	if (ove_sys_get_mem_stats(&memory) == OVE_OK) {
		char line[96];
		char *p = put_str(line, "[heap] free=");
		p = put_dec(p, (uint32_t)memory.free);
		p = put_str(p, " peak_used=");
		p = put_dec(p, (uint32_t)memory.peak_used);
		p = put_str(p, " total=");
		p = put_dec(p, (uint32_t)memory.total);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(line);
	}
	ove_lxp_console_write("[stack] end\n");
	ove_time_delay_ms(50);
}
