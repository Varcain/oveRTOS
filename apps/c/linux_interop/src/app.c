/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * RTOS-kernel <-> Linux-personality interop demo.
 *
 * One firmware image, two worlds, two phases, using only engine-neutral
 * oveRTOS thread, time, socket, console, and Linux-host APIs.
 *
 *  Phase 1 — BIDIRECTIONAL round trip. A native RTOS thread (ove_thread) feeds
 *  three readings into the rootfs-owned guest mode and drains its replies:
 *      RTOS feeder -> read cb -> guest roundtrip -> write cb -> RTOS consumer
 *
 *  Phase 2 — INTERACTIVE shell. The same rootfs entrypoint boots userspace;
 *  type commands (ls /, echo hi, cat /etc/hostname, ...) and
 *  `exit` to finish.
 *
 * Guest callbacks run in the privileged, preemptible coordinator. Fixed staging
 * keeps the native round trip allocation-free.
 */

#include "ove/lxp_console.h"
#include "ove/thread.h"

#include "ove/app.h"

#include "ove_config.h"
#include "ove/build.h" /* OVE_BUILD_ID — generated revisions with honest fallbacks */
#include "network_smoke.h"
#include "qualification.h"
#include "roundtrip.h"
#include "rt_scope.h"

#define GUEST_ENTRYPOINT "/usr/libexec/ove-interop-guest"

/* One parsed host is reused by both rootfs guest modes. */
static ove_lxp_host_t g_linux_host;

static int run_guest_mode(const ove_lxp_launch_config_t *config, const char *mode)
{
	const char *const argv[] = {"ove-interop-guest", mode, NULL};
	return ove_lxp_host_run(&g_linux_host, config, GUEST_ENTRYPOINT, 2, argv);
}

static void demo_exit(unsigned int code)
{
	ove_lxp_host_deinit(&g_linux_host);
	ove_app_exit(code);
}

static ove_thread_t g_demo;
/* The O0 QEMU integration build uses 4320 bytes of this coordinator stack;
 * retain nearly another call-chain of margin in backend-safe host memory. */
OVE_THREAD_DEFINE_HOST(g_demo_storage, 8192);

static void demo_body(void *arg)
{
	(void)arg;
	(void)ove_lxp_console_init();
	ove_lxp_console_write(
		"=== oveRTOS demo: a native RTOS thread + a Linux program, two-way ===\n");
	ove_lxp_console_write("[build] " OVE_BUILD_ID "\n");

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	if (linux_rt_scope_start(ove_lxp_console_write) != OVE_OK) {
		ove_lxp_console_write("[rt-scope] FAIL: timer/event/thread setup\n");
		demo_exit(1);
	}
	ove_lxp_console_write("[rt-scope] CH1=D3/PB4 TIM3 1kHz reference; "
			      "CH2=D4/PG7 critical-thread response\n");
#endif

	linux_interop_qualification_start();

	/* Build configuration owns rootfs placement and product network topology;
	 * the host facade owns native provider setup, rollback, and teardown. */
	int host_rc = ove_lxp_host_init(&g_linux_host);
	if (host_rc != OVE_OK) {
		ove_lxp_console_printf("[demo] FAIL: Linux host init failed rc=%d\n", host_rc);
		demo_exit(1);
	}

	linux_interop_network_report(&g_linux_host);

	/* ---- Phase 1: rootfs-owned bidirectional round trip ------------------- */
	ove_lxp_console_write("\n-- phase 1: RTOS thread <-> Linux program (bidirectional) --\n");
	ove_lxp_launch_config_t cfg1 = {
		.rt_scope_read = linux_rt_scope_proc_read,
	};
	int roundtrip_rc = linux_interop_roundtrip_prepare(&cfg1);
	if (roundtrip_rc != OVE_OK) {
		ove_lxp_console_printf("[demo] FAIL: round-trip worker init rc=%d\n", roundtrip_rc);
		demo_exit(1);
	}
	ove_lxp_console_bind_diagnostics(&cfg1);
	ove_lxp_console_write("[demo] launching the Linux guest round-trip mode...\n");
	int rc1 = run_guest_mode(&cfg1, "roundtrip");
	if (linux_interop_roundtrip_complete(rc1) != OVE_OK)
		demo_exit(1);

	linux_interop_network_smoke(&g_linux_host);

	linux_interop_qualification_arm_guest_tests();

	/* ---- Phase 2: boot userspace or run the hard-float context regression - */
#if defined(CONFIG_OVE_LINUX_GUEST_FP_SELFTEST)
	ove_lxp_console_write("\n-- phase 2: hard-float guest context self-test --\n");
#else
	ove_lxp_console_write("\n-- phase 2: booting uClinux (BusyBox init -> rcS -> login shell;"
			      " run commands, `poweroff` to halt) --\n");
#endif
	ove_lxp_launch_config_t cfg2 = {
		.rt_scope_read = linux_rt_scope_proc_read,
	};
	ove_lxp_console_bind_diagnostics(&cfg2);
	ove_lxp_console_bind(&cfg2);
	int rc2;
	if (linux_interop_qualification_measurement_start() != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: latency monitor thread init\n");
		demo_exit(1);
	}
#if defined(CONFIG_OVE_LINUX_GUEST_FP_SELFTEST)
	rc2 = run_guest_mode(&cfg2, "fpcheck");
	ove_lxp_console_write("\n=== interop demo done (hard-float self-test exited) ===\n");
#else
	rc2 = run_guest_mode(&cfg2, "boot");
	ove_lxp_console_write("\n=== interop demo done (uClinux halted) ===\n");
#endif
	linux_interop_qualification_measurement_stop();
	ove_lxp_host_observation_t observation;
	if (ove_lxp_host_observe(&g_linux_host, &observation) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: post-run LXP observation unavailable\n");
		demo_exit(1);
	}
	linux_interop_qualification_report(&observation, g_demo, sizeof(g_demo_storage_stack));
	demo_exit(rc2 >= 0 ? 0 : 1);
}

void ove_main(void)
{
	if (ove_thread_init(&g_demo, &g_demo_storage, "demo", demo_body, NULL, OVE_PRIO_NORMAL,
			    sizeof(g_demo_storage_stack), g_demo_storage_stack) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: demo thread init\n");
		demo_exit(1);
	}
	ove_run();
}
