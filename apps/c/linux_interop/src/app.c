/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * RTOS-kernel <-> Linux-personality interop demo (mps2/an521, Cortex-M33).
 *
 * One firmware image, two worlds, two phases — built entirely on the
 * engine-agnostic oveRTOS APIs (ove_thread / ove_queue / ove_time) on the RTOS
 * side and the Linux-personality runner (lxp_run) on the Linux side; no
 * direct Zephyr kernel calls.
 *
 *  Phase 1 — BIDIRECTIONAL round trip. A native RTOS thread (ove_thread) feeds
 *  three "sensor readings" INTO a stock Linux program (BusyBox `cat`, an
 *  unprivileged uClibc FDPIC) through its stdin, and drains what it echoes back
 *  OUT of its stdout:
 *      RTOS feeder -> g_feed_lines[] -> read cb -> [Linux cat] -> write cb -> g_round_trip[] -> RTOS consumer
 *
 *  Phase 2 — INTERACTIVE shell. The program then drops into an interactive
 *  BusyBox `sh`; type commands (ls /, echo hi, cat /etc/hostname, ...) and
 *  `exit` to finish.
 *
 * The svc top half only snapshots ordinary syscall registers and parks the guest;
 * I/O callbacks run later in the privileged, preemptible coordinator task. Phase 1
 * keeps its fixed arrays and published indices to avoid allocation and unnecessary
 * scheduler traffic. Phase 2 binds the oveRTOS system-console provider, whose
 * non-consuming readiness probe parks an empty console instead of blocking the
 * coordinator and whose event-capable backends wake it without periodic polling.
 */

#include <string.h>

#include "ove/lxp_console.h"
#include "ove/lxp_host.h"
#include "ove/thread.h"
#include "ove/time.h"

#include "ove/app.h"
#include "lxp/lxp_config.h" /* LXP_NSLOT (the latency report walks the slots) */
#include "lxp/lxp_latency.h"
#if defined(CONFIG_OVE_LINUX_NET)
#include "ove/net.h" /* product-level socket smoke over the initialized host network */
#endif
#if defined(CONFIG_OVE_WATCHDOG)
#include "ove/watchdog.h" /* host-owned IWDG feed */
#include "ove/reset.h"	  /* why the last reset happened (watchdog recovery is visible) */
#endif

#include "ove_config.h" /* CONFIG_OVE_RTOS_FREERTOS — selects the app lifecycle below */
#include "ove/build.h" /* OVE_BUILD_ID — generated revisions with honest fallbacks */
#include "rt_scope.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS)
#include "lxp/ports/freertos.h"
#endif

#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI) || \
	defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500) || \
	defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
#include "ove/lxp_memory_layout.h"
#define LXP_EXTERNAL_ROOTFS ((const uint8_t *)OVE_LXP_ROOTFS_BASE)
#define LXP_EXTERNAL_ROOTFS_MAX ((size_t)OVE_LXP_ROOTFS_SIZE)
#else
#include "loader_rootfs_image.h" /* ove_test_rootfs_cpio[], _len — a real Buildroot rootfs */
#endif

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/* ---- product exit policy --------------------------------------------------- */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static void sh_exit(unsigned int code)
{
	(void)code;
	/* No semihosting on bare metal: poweroff/halt → system reset (back to the boot banner).
	 * SCB->AIRCR = VECTKEY(0x05FA) | SYSRESETREQ(bit 2). */
	*(volatile unsigned int *)0xE000ED0Cu = 0x05FA0004u;
	__asm__ volatile("dsb 0xf" ::: "memory");
	for (;;) {
	}
}
#else
/* SYS_EXIT_EXTENDED gives QEMU a clean product-level termination. */
static long semihost(unsigned long op, void *arg)
{
	register unsigned long r0 __asm__("r0") = op;
	register void *r1 __asm__("r1") = arg;
	__asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
	return (long)r0;
}
static void sh_exit(unsigned int code)
{
	unsigned long block[2] = {0x20026u /* ADP_Stopped_ApplicationExit */, code};
	semihost(0x20 /* SYS_EXIT_EXTENDED */, block);
	for (;;) {
	}
}
#endif

/* Minimal string builders (no libc printf dependency). */
static char *put_str(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *put_dec(char *p, uint32_t v)
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

static char *put_sdec(char *p, int32_t v)
{
	if (v < 0) {
		*p++ = '-';
		return put_dec(p, (uint32_t)(-(int64_t)v));
	}
	return put_dec(p, (uint32_t)v);
}

static char *put_hex32(char *p, uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	*p++ = '0';
	*p++ = 'x';
	for (int shift = 28; shift >= 0; shift -= 4)
		*p++ = hex[(v >> shift) & 0xfu];
	return p;
}

static uint32_t uptime_ms(void)
{
	uint64_t us = 0;
	(void)ove_time_get_us(&us);
	return (uint32_t)(us / 1000u);
}

#if defined(CONFIG_OVE_LINUX_NET)
/*
 * Run after phase 1 and allow a bounded readiness window. RTOS network seams
 * publish the static address before their carrier, worker, and ARP paths
 * necessarily become connect-ready. A one-shot probe therefore describes a
 * scheduler race rather than transport health, especially on NuttX.
 */
static void network_transport_smoke(void)
{
	static ove_socket_storage_t s_sk;
	ove_sockaddr_t peer;
	ove_sockaddr_ipv4(&peer, 172, 1, 1, 1, 22);
	const uint32_t started_ms = uptime_ms();
	int last_rc = -1;
	for (uint32_t attempt = 1; attempt <= 12; attempt++) {
		ove_socket_t sk = NULL;
		last_rc = ove_socket_open(&sk, &s_sk, OVE_AF_INET, OVE_SOCK_STREAM);
		if (last_rc != OVE_OK)
			goto retry;

		last_rc = ove_socket_connect(sk, &peer, OVE_MS(500));
		if (last_rc != OVE_OK) {
			ove_socket_close(sk);
			goto retry;
		}

		char rb[80];
		size_t got = 0;
		last_rc = ove_socket_recv(sk, rb, sizeof(rb) - 1, &got, OVE_SEC(1));
		if (last_rc == OVE_OK && got > 0) {
			for (size_t i = 0; i < got; i++)
				if (rb[i] == '\r' || rb[i] == '\n')
					rb[i] = 0;
			rb[got < sizeof(rb) ? got : sizeof(rb) - 1] = 0;
			char b[176];
			char *p = put_str(b,
					  "[demo] socket smoke (post-phase1) OK <- 172.1.1.1:22: ");
			p = put_str(p, rb);
			p = put_str(p, " (ready after ");
			p = put_dec(p, uptime_ms() - started_ms);
			p = put_str(p, " ms, attempt ");
			p = put_dec(p, attempt);
			*p++ = ')';
			*p++ = '\n';
			*p = 0;
			ove_lxp_console_write(b);
			ove_socket_close(sk);
			return;
		}
		ove_socket_close(sk);

retry:
		if (attempt < 12)
			ove_thread_sleep_ms(250);
	}

	char b[112];
	char *p = put_str(b, "[demo] socket smoke (post-phase1): not ready after ");
	p = put_dec(p, uptime_ms() - started_ms);
	p = put_str(p, " ms, last rc=");
	p = put_sdec(p, last_rc);
	*p++ = '\n';
	*p = 0;
	ove_lxp_console_write(b);
}
#endif

/* ---- the RTOS <-> Linux bridges: two oveRTOS message queues ---------------- */
struct lnx_line {
	char text[56];
};
#define N_READINGS 3

/* Phase-1 I/O uses fixed arrays instead of queues. feed_read/consume_write now run in the
 * privileged coordinator task, but the pre-staged array keeps the path allocation-free and makes
 * an empty feed unambiguously mean EOF. The native worker thread + two-way data flow are preserved. */
static struct lnx_line g_feed_lines[N_READINGS]; /* RTOS -> Linux: readings staged up front */
static volatile int g_feed_idx;			 /* feed_read cursor; == N_READINGS => EOF */
static volatile int g_feed_ready;	  /* all feed lines queued (so no premature EOF) */
static volatile int g_linux_done;	  /* the phase-1 program has exited */
static volatile int g_worker_exited;	  /* the worker thread has returned */
static char g_round_trip[N_READINGS][56]; /* what came back through Linux (for the verdict) */
static volatile int g_round_trip_n;

/* ---- the native RTOS worker thread (feeds, then consumes) ------------------ */
static ove_thread_t g_worker;
static ove_thread_storage_t g_worker_storage;
/* Aligned to its own (power-of-2) size: Zephyr USERSPACE on a power-of-2 MPU (PMSAv7, e.g. the
 * STM32F746) requires thread-stack objects to be power-of-2 aligned+sized, else k_thread_create
 * rounds the base up to Z_POW2_CEIL(size) and overruns the buffer. Harmless on other engines. */
static uint8_t g_worker_stack[2048] __attribute__((aligned(2048)));

static void rtos_worker(void *arg)
{
	UNUSED(arg);

	/* RTOS -> Linux: stage all the readings up front into a plain array; feed_read (in the guest's
	 * SVC handler) serves them by index. The program cannot block waiting for input, so pre-staging
	 * also guarantees "empty == genuine EOF". */
	for (int i = 1; i <= N_READINGS; i++) {
		char *p = put_str(g_feed_lines[i - 1].text, "reading-");
		p = put_dec(p, (uint32_t)i);
		*p++ = '\n'; /* the program reads a line at a time */
		*p = 0;
		char b2[40];
		char *q = put_str(b2, "[rtos-feeder] -> Linux: reading-");
		q = put_dec(q, (uint32_t)i);
		*q++ = '\n';
		*q = 0;
		ove_lxp_console_write(b2);
	}
	g_feed_ready = 1;

	/* Linux -> RTOS: print each reply the moment consume_write records it — concurrently with the
	 * running program. consume_write publishes g_round_trip_n AFTER the text, so any count we read
	 * here has its text fully written. */
	int printed = 0;
	for (;;) {
		while (printed < g_round_trip_n) {
			char line[96];
			char *p = put_str(line, "[rtos-consumer] <- Linux (round trip #");
			p = put_dec(p, (uint32_t)(printed + 1));
			p = put_str(p, " @ ");
			p = put_dec(p, uptime_ms());
			p = put_str(p, " ms): \"");
			p = put_str(p, g_round_trip[printed]);
			p = put_str(p, "\"\n");
			*p = 0;
			ove_lxp_console_write(line);
			printed++;
		}
		if (g_linux_done && printed >= g_round_trip_n)
			break;
		/* Poll coarsely (not a tight spin): the worker must stay fully idle across the whole load
		 * window so it neither preempts nor churns the scheduler while demo_body reads the program
		 * image from the QUADSPI (see the OVE_PRIO_LOW note above). 50 ms comfortably spans the load;
		 * the replies then print in a small burst — correct, just not one-at-a-time. */
		ove_time_delay_ms(50);
	}
	g_worker_exited = 1;
}

/* ---- phase 1 callbacks: round-trip through the oveRTOS queues --------------- */
/* stdin: hand the program the next RTOS-produced line; empty queue == EOF (the
 * feeder pre-filled it, so "empty" really does mean the readings are exhausted). */
static long feed_read(void *ctx, int fd, void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
	if (g_feed_idx >= N_READINGS)
		return 0; /* EOF: every staged reading has been served */
	const char *src = g_feed_lines[g_feed_idx++].text;
	size_t l = strlen(src);
	if (l > len)
		l = len;
	memcpy(buf, src, l);
	return (long)l;
}

/* stdout: push each line the program emits to the RTOS consumer (ISR-safe). */
static long consume_write(void *ctx, int fd, const void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
	char tmp[56];
	size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
	memcpy(tmp, buf, n);
	while (n && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r'))
		n--;
	tmp[n] = 0;
	if (n) {
		int idx = g_round_trip_n;
		if (idx < N_READINGS) {
			memcpy(g_round_trip[idx], tmp, n + 1);
			__asm__ volatile("" ::: "memory"); /* publish the text before the count */
			g_round_trip_n = idx + 1; /* the worker prints replies as this advances */
		}
	}
	return (long)len;
}

static void on_enosys(long nr)
{
	char b[40];
	char *p = put_str(b, "[demo] unimplemented syscall nr=");
	p = put_dec(p, (uint32_t)nr);
	*p++ = '\n';
	*p = 0;
	ove_lxp_console_write(b);
}

static const char *exit_reason_name(uint8_t reason)
{
	switch (reason) {
	case LXP_EXIT_REASON_SIGNAL:
		return "signal";
	case LXP_EXIT_REASON_SIGNAL_DEPTH:
		return "signal-depth";
	case LXP_EXIT_REASON_MEMORY_FAULT:
		return "memory-fault";
	case LXP_EXIT_REASON_EXEC_RESOURCE:
		return "exec-resource";
	case LXP_EXIT_REASON_EXEC_LOAD:
		return "exec-load";
	case LXP_EXIT_REASON_STATE_CORRUPTION:
		return "state-corruption";
	default:
		return "unspecified";
	}
}

/* Development-target attribution for contained guest failures. Normal exits stay
 * silent; abnormal records are emitted from coordinator task context, never from
 * an exception handler, and remain bounded to one short UART line. */
static void on_guest_exit(const lxp_guest_exit_info_t *info)
{
	if (!info || info->reason == LXP_EXIT_REASON_NORMAL)
		return;
	char b[192];
	char *p = put_str(b, "[lxp] guest-exit slot=");
	p = put_dec(p, (uint32_t)info->slot);
	p = put_str(p, " pid=");
	p = put_dec(p, (uint32_t)info->pid);
	p = put_str(p, " comm=");
	p = put_str(p, info->comm ? info->comm : "?");
	p = put_str(p, " status=");
	p = put_dec(p, (uint32_t)info->status);
	p = put_str(p, " reason=");
	p = put_str(p, exit_reason_name(info->reason));
	if (info->signal) {
		p = put_str(p, " signal=");
		p = put_dec(p, info->signal);
	}
	if (info->detail) {
		p = put_str(p, " detail=");
		p = put_hex32(p, info->detail);
	}
	if (info->address) {
		p = put_str(p, " address=");
		p = put_hex32(p, (uint32_t)info->address);
	}
	*p++ = '\n';
	*p = 0;
	ove_lxp_console_write(b);
}

/* ---- rootfs (parsed from the board-selected Buildroot CPIO backing) -------- */
#define ROOTFS_MAX_FILES 512
static lxp_file_t g_rootfs[ROOTFS_MAX_FILES];
/* NuttX must finish early driver registration from the SRAM1 tail before it can
 * add SRAM2 and DTCM to the heap. Keep the STM32 pathname copy within the
 * current image's measured 11,101 bytes plus useful growth margin; reserving
 * 16 KiB here leaves g_idle_topstack beyond SRAM1 after the personality's
 * per-process state is linked, corrupting the initial allocator before boot. */
#if defined(CONFIG_OVE_RTOS_NUTTX) && defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#define ROOTFS_NAME_BYTES (12 * 1024)
#else
#define ROOTFS_NAME_BYTES (16 * 1024)
#endif
static char g_rootfs_names[ROOTFS_NAME_BYTES];
static ove_lxp_host_t g_linux_host;

static void demo_exit(unsigned int code)
{
	ove_lxp_host_deinit(&g_linux_host);
	sh_exit(code);
}

/* The engine-agnostic demo. On FreeRTOS the scheduler starts inside ove_run(), so
 * this must run in a task; Zephyr and NuttX call ove_main() from running scheduler
 * contexts and execute it inline. ove_main() below wires that lifecycle. */
#ifdef CONFIG_OVE_RTOS_FREERTOS
static ove_thread_t g_demo;
static ove_thread_storage_t g_demo_storage;
/* Deferred syscalls execute on this coordinator stack. The O0 QEMU integration build reaches
 * 4320 bytes while loading and running BusyBox, so retain nearly another full call-chain of
 * margin. On STM32 this is a critical host stack and must remain in internal SRAM: the privileged
 * coordinator intentionally sees guest SDRAM through the uncached Device background mapping,
 * which is suitable for controlled aligned buffer accesses but not for compiler-generated stack
 * accesses or exception/FP stacking. Higher-priority host tasks still preempt this task normally. */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static uint8_t g_demo_stack[8192]
	__attribute__((section(".host_stacks"), aligned(32)));
#else
static uint8_t g_demo_stack[8192] __attribute__((aligned(8)));
#endif
#endif

#if defined(CONFIG_OVE_WATCHDOG)
/* ---- host watchdog: a high-priority task owns the IWDG feed -------------------------------
 * The IWDG (LSI-clocked, independent of the core clock) resets the board if it is not fed within
 * its timeout. This monitor at OVE_PRIO_HIGH owns the feed — above the coordinator (NORMAL) and
 * the guest slots (IDLE+1). What it feeds ON is host liveness only:
 *   - while the coordinator is not driving a guest, the monitor running at all proves the
 *     scheduler is alive, which is the whole guarantee available in that window;
 *   - while it is, the monitor feeds only if the coordinator's heartbeat advanced since the last
 *     check. A stalled heartbeat with a live scheduler is a wedged coordinator, which a plain
 *     unconditional high-priority feed would miss.
 * Guest progress never enters the decision: a faulting guest is contained by the MPU, not a reset
 * trigger, and a guest must not be able to hold the watchdog open.
 *
 * The IWDG timeout (not a counter here) is the tolerance for a transient non-advancing window: the
 * longest single coordinator dispatch measured — a ~105 ms fork (R7) — is ~19x under 2 s, so a
 * legitimate long dispatch never resets while a real stall (unfed > 2 s) always does. */
#define WD_TIMEOUT_MS 2000u
#define WD_FEED_MS 250u

static ove_thread_t g_wd;
static ove_thread_storage_t g_wd_storage;
/* Sized per build like the latency monitor: -O0 spills more (see CONFIG_OVE_DEBUG_BUILD). .bss
 * keeps it in internal SRAM, which a host task stack needs on STM32. The R9 soak's stack audit
 * measured only 128 B used at -Os, so 512 B (384 B free) is comfortable. */
#if defined(CONFIG_OVE_DEBUG_BUILD)
static uint8_t g_wd_stack[1024] __attribute__((aligned(1024)));
#else
static uint8_t g_wd_stack[512] __attribute__((aligned(512)));
#endif
static ove_watchdog_t g_wd_dog;
static ove_watchdog_storage_t g_wd_dog_storage;

/* The feed decision, named for what it encodes: feed iff the coordinator is not running a guest,
 * or its heartbeat advanced since the last look. The self-test below exercises the withhold branch
 * (active && !advanced) end to end against real hardware. */
static int wd_should_feed(int active, int hb_advanced)
{
	return !active || hb_advanced;
}

#if defined(CONFIG_OVE_WATCHDOG_SELFTEST)
/* Debug-gated proof that the policy resets a wedged host. A spinner at OVE_PRIO_ABOVE_NORMAL —
 * between the coordinator (NORMAL) and the monitor (HIGH) — starves the coordinator so its
 * heartbeat freezes, WITHOUT masking interrupts, so the scheduler stays live and the monitor keeps
 * running, sees the stall, withholds the feed, and the IWDG resets the board. That exercises the
 * heartbeat-gating path end to end, not merely scheduler death. One-shot: skipped when this boot
 * came FROM a watchdog reset, so it trips once and then the board runs clean. */
static ove_thread_t g_wdtest;
static ove_thread_storage_t g_wdtest_storage;
static uint8_t g_wdtest_stack[512] __attribute__((aligned(512)));

static void wdtest_spin(void *arg)
{
	UNUSED(arg);
	for (;;)
		__asm volatile("nop"); /* no __disable_irq: starve the coordinator, keep the scheduler */
}

static void wd_selftest_maybe_trip(void)
{
	if (ove_reset_cause() == OVE_RESET_WATCHDOG) {
		ove_lxp_console_write("[wd] selftest: recovered from the watchdog reset; not re-tripping\n");
		return;
	}
	ove_lxp_console_write("[wd] selftest: starving the coordinator (scheduler stays live);"
		  " expect a watchdog reset in ~2s...\n");
	(void)ove_thread_init(&g_wdtest, &g_wdtest_storage, "wdtest", wdtest_spin, NULL,
			      OVE_PRIO_ABOVE_NORMAL, sizeof(g_wdtest_stack), g_wdtest_stack);
}
#endif /* CONFIG_OVE_WATCHDOG_SELFTEST */

static void wd_body(void *arg)
{
	UNUSED(arg);
	if (ove_watchdog_init(&g_wd_dog, &g_wd_dog_storage, WD_TIMEOUT_MS) != OVE_OK ||
	    ove_watchdog_start(g_wd_dog) != OVE_OK) {
		/* Could not arm. Running unguarded beats spinning here — a spin would be the very
		 * kind of wedge nothing is left to catch. */
		ove_lxp_console_write("[wd] FAIL: could not arm IWDG; running without a watchdog\n");
		return;
	}
	ove_lxp_console_write("[wd] IWDG armed: 2000ms timeout, fed every 250ms while the host stays live\n");

	uint32_t last = 0;
	int primed = 0;
#if defined(CONFIG_OVE_WATCHDOG_SELFTEST)
	unsigned cycle = 0;
	int tripped = 0;
#endif
	for (;;) {
		lxp_run_health_t h;
		lxp_run_health(&h);
		int feed = !primed || wd_should_feed(h.active, h.coord_iters != last);
		last = h.coord_iters;
		primed = 1;
		if (feed)
			(void)ove_watchdog_feed(g_wd_dog);
#if defined(CONFIG_OVE_WATCHDOG_SELFTEST)
		/* Trip once the guest is actually running (~4 s in), so the wedge stalls a live
		 * coordinator rather than an idle one. */
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
/* Debug-gated proof (C6) that a fault in host/privileged context is fatal, never mis-contained as a
 * guest fault. A privileged host task (created like any other via ove_thread_init, so current_slot()
 * cannot match it) executes an undefined instruction a few seconds in, while a guest is running. The
 * seam's fault handler sees no owning slot, declines containment, prints a HOST FAULT diagnostic and
 * halts; the watchdog resets. One-shot: skipped when this boot came FROM a watchdog reset. */
static ove_thread_t g_ftest;
static ove_thread_storage_t g_ftest_storage;
static uint8_t g_ftest_stack[512] __attribute__((aligned(512)));

static void ftest_body(void *arg)
{
	UNUSED(arg);
	ove_thread_sleep_ms(4000); /* let phase 2 publish the active personality run */
	ove_lxp_console_write("[c6] faulting a privileged host task (udf) while a guest runs;"
		  " expect HOST FAULT + watchdog reset\n");
	__asm volatile("udf #0"); /* host UsageFault -> LXP port invokes the fatal host callback */
	for (;;) { /* unreachable */
	}
}

static void faulttest_maybe_arm(void)
{
	if (ove_reset_cause() == OVE_RESET_WATCHDOG) {
		ove_lxp_console_write("[c6] recovered from the host-fault test; not re-arming\n");
		return;
	}
	(void)ove_thread_init(&g_ftest, &g_ftest_storage, "ftest", ftest_body, NULL, OVE_PRIO_NORMAL,
			      sizeof(g_ftest_stack), g_ftest_stack);
}
#endif /* CONFIG_OVE_LINUX_FAULTTEST */

#if defined(CONFIG_OVE_LINUX_SMASHTEST)
/* Debug-gated proof (C9) that a stack smash reaches the board's __stack_chk_fail, not picolibc's
 * silent default. smash_host_stack overflows a local buffer past the canary -fstack-protector-strong
 * placed above it; its epilogue's canary check then calls __stack_chk_fail (print + halt), and the
 * watchdog resets. One-shot: skipped when this boot came FROM a watchdog reset. */
static ove_thread_t g_smash;
static ove_thread_storage_t g_smash_storage;
static uint8_t g_smash_stack[512] __attribute__((aligned(512)));

/* noinline so its OWN epilogue runs the canary check right after the overflow — inlined into the
 * caller, the overflow would land far from the caller's canary and never trip. Writing through a
 * volatile pointer keeps the overflow out of the compiler's array-bounds analysis (which would
 * -Werror) and the barrier stops it being elided. The overflow stays within this task's stack, so
 * the canary check — not a wild access — is what trips. */
static __attribute__((noinline)) void smash_host_stack(void)
{
	volatile char buf[16];
	volatile char *p = buf;
	for (int i = 0; i < 40; i++)
		p[i] = (char)(0xa5 + i); /* i >= 16 overruns buf into the canary */
	__asm__ volatile("" : : "r"(p) : "memory");
}

static void smashtest_body(void *arg)
{
	UNUSED(arg);
	ove_thread_sleep_ms(4500); /* just after the C6 fault test, if both are on; guest is up */
	ove_lxp_console_write("[c9] smashing a host stack buffer; expect STACK SMASH + watchdog reset\n");
	smash_host_stack(); /* returns through a smashed canary -> __stack_chk_fail */
	for (;;) { /* unreachable */
	}
}

static void smashtest_maybe_arm(void)
{
	if (ove_reset_cause() == OVE_RESET_WATCHDOG) {
		ove_lxp_console_write("[c9] recovered from the smash test; not re-arming\n");
		return;
	}
	(void)ove_thread_init(&g_smash, &g_smash_storage, "smash", smashtest_body, NULL,
			      OVE_PRIO_NORMAL, sizeof(g_smash_stack), g_smash_stack);
}
#endif /* CONFIG_OVE_LINUX_SMASHTEST */

#if LXP_ENABLE_LATENCY
/* ---- host deadline monitor (measurement builds only) -------------------------------------
 * A periodic OVE_PRIO_HIGH task, i.e. above both the coordinator (OVE_PRIO_NORMAL, this task)
 * and the guest slots (tskIDLE_PRIORITY+1). Being top of the ladder is the point: everything
 * it still gets delayed by is delay that priority cannot fix. On FreeRTOS that is the
 * interrupt-masked window of the coordinator's taskENTER_CRITICAL(), plus ISRs and tick
 * quantisation — a guest cannot preempt this task, so what is left is the honest measure of
 * how much a guest's activity can push out a host deadline.
 *
 * Recorded as overshoot of the requested sleep (woken_at - slept_for - period), which folds in
 * tick quantisation: on a 1 kHz tick a 10 ms sleep may legitimately return up to ~1 tick late,
 * so expect a nonzero floor. That floor is the baseline the guest-induced tail sits on top of.
 *
 * Deliberately no deadline and no miss counter: a miss needs a bound to miss, and the bound is
 * what this run exists to inform. Count + max + histogram only.
 */
#define MON_PERIOD_MS 10u

static ove_thread_t g_mon;
static ove_thread_storage_t g_mon_storage;
/* Power-of-2 sized+aligned for the same reason as g_worker_stack. Plain .bss keeps it in
 * internal SRAM, which a host task stack requires on STM32 (see g_demo_stack: the coordinator
 * maps guest SDRAM as uncached Device memory, which does not suit compiler-generated stack
 * access) — so this comes out of the scarcest region and is sized accordingly. The body holds
 * four u64s, calls only ove_time_get_ns/ove_thread_sleep_ms, and never prints: demo_body writes
 * the report once the monitor has stopped. Cortex-M ISRs stack on MSP, not this PSP.
 *
 * Sized per build, not once: -O0 spills every local and inlines nothing, so the same body wants
 * materially more stack than the optimized build. 512 B is enough optimized (over seven hardware
 * runs configCHECK_FOR_STACK_OVERFLOW=2 never tripped, and this task switches out ~6300 times a
 * run) but overflows at -O0 — where the default vApplicationStackOverflowHook spins silently, so
 * it presents as the system simply stopping rather than as a diagnosable fault. Charging the
 * debug figure to every build would cost the STM32 1 KB of internal SRAM it does not have: with
 * the counters in, RAM is left with ~2.3 KB above a 2 KB floor. */
#if defined(CONFIG_OVE_DEBUG_BUILD)
static uint8_t g_mon_stack[1024] __attribute__((aligned(1024)));
#else
static uint8_t g_mon_stack[512] __attribute__((aligned(512)));
#endif
static lxp_lat_stat_t g_mon_late;
static volatile int g_mon_stop;
static volatile int g_mon_exited;

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
		lxp_lat_record(&g_mon_late, slept > want ? slept - want : 0);
	}
	g_mon_exited = 1;
}

static void lat_row(const char *what, const char *name, const lxp_lat_stat_t *s)
{
	if (!s || !s->count)
		return; /* a class never dispatched has nothing to say; skip the row */
	char line[192];
	char *p = put_str(line, "[lat] ");
	p = put_str(p, what);
	*p++ = ' ';
	p = put_str(p, name);
	while ((p - line) < 30)
		*p++ = ' ';
	p = put_str(p, "n=");
	p = put_dec(p, s->count);
	p = put_str(p, " max_ns=");
	p = put_dec(p, s->max_ns);
	p = put_str(p, " us[");
	for (int b = 0; b < LXP_LAT_BUCKETS; b++) {
		if (b)
			*p++ = ' ';
		p = put_dec(p, s->buckets[b]);
	}
	p = put_str(p, "]\n");
	*p = 0;
	ove_lxp_console_write(line);
}

static void lat_report(void)
{
	ove_lxp_console_write("\n=== latency (measurement build; no threshold is enforced) ===\n"
		  "[lat] us[] buckets: <1 <2 <4 <8 <16 <32 <64 >=64\n");
	lat_row("host-wake-overshoot", "", &g_mon_late);
	for (int c = 1; c < LXP_LAT_CLASSES; c++)
		lat_row("coord-service", lxp_lat_class_name(c), lxp_lat_service_get(c));
	for (int s = 0; s < LXP_NSLOT; s++) {
		char nm[8];
		char *p = put_dec(nm, (uint32_t)s);
		*p = 0;
		lat_row("guest-wake slot", nm, lxp_lat_wake_get(s));
	}
	/* Terminator. Rows for classes/slots that saw nothing are skipped, so a report cut short
	 * is indistinguishable from one that simply had less to say — and a truncated report
	 * understates exactly the maxima it exists to show. The drivers require this line. The
	 * delay lets the UART shift out: the caller's exit path resets the part, which would
	 * otherwise drop whatever is still in flight (this line included). */
	ove_lxp_console_write("[lat] end\n");
	ove_time_delay_ms(50);
}
#endif /* LXP_ENABLE_LATENCY */

/* ---- teardown stack + heap audit (R9 / C3) -----------------------------------------------
 * Printed after the guest has run (and, in a soak, after the stress workload), so the high-water
 * marks reflect the deepest paths actually taken — an idle demo never reaches them, which is why
 * R2's stack criterion was unmet. The coordinator (this task, g_demo) is the one that matters: it
 * runs the program loader and the whole syscall dispatch. Free = size - high-water; a soak driver
 * fails the run if any free falls below its floor. */
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

/* ove_thread_get_stack_usage() returns the high-water FREE bytes (untouched sentinel space), not
 * used — consistent across all backends despite the name (see the note in ove/thread.h). Convert
 * to used for the audit so every row means the same thing. */
static void audit_thread(const char *name, ove_thread_t h, size_t size)
{
	size_t freeb = ove_thread_get_stack_usage(h);
	audit_stack_line(name, size > freeb ? size - freeb : 0, size);
}

/* Report only the high-water free margin for a thread whose total stack this app does not own —
 * the RTOS-provided ove_main thread the coordinator runs inline on under Zephyr/NuttX (there is no
 * app-sized g_demo_stack there). Free is still the number the soak floor cares about. */
#if !defined(CONFIG_OVE_RTOS_FREERTOS) /* used only in the non-FreeRTOS stack_audit() branch below */
static void audit_thread_free(const char *name, ove_thread_t h)
{
	char line[96];
	char *p = put_str(line, "[stack] ");
	p = put_str(p, name);
	while ((p - line) < 26)
		*p++ = ' ';
	p = put_str(p, "free=");
	p = put_dec(p, (uint32_t)ove_thread_get_stack_usage(h));
	*p++ = '\n';
	*p = 0;
	ove_lxp_console_write(line);
}
#endif /* !CONFIG_OVE_RTOS_FREERTOS */

static void lxp_diag_audit(void)
{
	lxp_diag_size_report_t sizes;
	lxp_diag_size_report(&sizes);
	{
		char line[224];
		char *p = put_str(line, "[lxp-size] slots=");
		p = put_dec(p, sizes.slots);
		p = put_str(p, " regions=");
		p = put_dec(p, sizes.regions);
		p = put_str(p, " proc=");
		p = put_dec(p, (uint32_t)sizes.proc);
		p = put_str(p, " slot-core=");
		p = put_dec(p, (uint32_t)sizes.per_slot_core);
		p = put_str(p, " region-core=");
		p = put_dec(p, (uint32_t)sizes.per_region_core);
		p = put_str(p, " slot-table=");
		p = put_dec(p, (uint32_t)sizes.slot_table);
		p = put_str(p, " coord-static=");
		p = put_dec(p, (uint32_t)sizes.coordinator_static);
		p = put_str(p, " exec-capture=");
		p = put_dec(p, (uint32_t)sizes.exec_capture);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(line);
	}
	{
		char line[224];
		char *p = put_str(line, "[lxp-size] mm=");
		p = put_dec(p, (uint32_t)sizes.mm);
		p = put_str(p, " files=");
		p = put_dec(p, (uint32_t)sizes.files);
		p = put_str(p, " fs=");
		p = put_dec(p, (uint32_t)sizes.fs);
		p = put_str(p, " sighand=");
		p = put_dec(p, (uint32_t)sizes.sighand);
		p = put_str(p, " group=");
		p = put_dec(p, (uint32_t)sizes.thread_group);
		p = put_str(p, " arena=");
		p = put_dec(p, (uint32_t)sizes.arena);
		p = put_str(p, " resume=");
		p = put_dec(p, (uint32_t)sizes.resume_context);
		p = put_str(p, " mailbox=");
		p = put_dec(p, (uint32_t)sizes.deferred_request);
		p = put_str(p, " signal=");
		p = put_dec(p, (uint32_t)sizes.signal_save_stack);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(line);
	}

	lxp_diag_health_t health;
	lxp_diag_health(&health);
	{
		char line[224];
		char *p = put_str(line, "[lxp-world] checks=");
		p = put_dec(p, health.checks);
		p = put_str(p, " failures=");
		p = put_dec(p, health.failures);
		if (health.failures) {
			p = put_str(p, " first=");
			p = put_str(p, lxp_diag_issue_name(health.first_error.issue));
			p = put_str(p, " slot=");
			p = put_sdec(p, health.first_error.slot);
			p = put_str(p, " region=");
			p = put_sdec(p, health.first_error.region);
			p = put_str(p, " last=");
			p = put_str(p, lxp_diag_issue_name(health.last_error.issue));
		}
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(line);
	}
}

static void stack_audit(void)
{
	lxp_diag_audit();
	ove_lxp_console_write("\n=== stack high-water audit (deepest usage this run) ===\n");
#if defined(CONFIG_OVE_RTOS_FREERTOS)
	/* FreeRTOS runs the coordinator in an app-owned task (g_demo/g_demo_stack). */
	audit_thread("coordinator", g_demo, sizeof(g_demo_stack));
#else
	/* Zephyr/NuttX run it inline on the ove_main thread — audit self, free-margin only. */
	audit_thread_free("coordinator", ove_thread_get_self());
#endif
	audit_thread("worker", g_worker, sizeof(g_worker_stack));
#if defined(CONFIG_OVE_WATCHDOG)
	audit_thread("wd-monitor", g_wd, sizeof(g_wd_stack));
#endif
#if LXP_ENABLE_LATENCY
	audit_thread("lat-monitor", g_mon, sizeof(g_mon_stack));
#endif
#if defined(CONFIG_OVE_RTOS_FREERTOS)
	/* Guest-slot tramp stack (192 words = 768 B in the LXP port): worst across all
	 * slots this run. The remaining 256 bytes of each aligned 1K allocation hold the persistent
	 * resume descriptor and are not part of the task stack. */
	audit_stack_line("guest-slot(tramp)", lxp_freertos_slot_stack_high_water_mark(), 768u);
#endif
	/* peak_used is the high-water of heap usage over the whole run, so it captures a cumulative
	 * leak (it would climb toward total across a soak) — a better single number than a boot-vs-end
	 * delta, which heap_4 spoils anyway by initialising lazily (free reads 0 before the first
	 * malloc, and all tasks here are static). */
	struct ove_mem_stats m;
	if (ove_sys_get_mem_stats(&m) == OVE_OK) {
		char line[96];
		char *p = put_str(line, "[heap] free=");
		p = put_dec(p, (uint32_t)m.free);
		p = put_str(p, " peak_used=");
		p = put_dec(p, (uint32_t)m.peak_used);
		p = put_str(p, " total=");
		p = put_dec(p, (uint32_t)m.total);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(line);
	}
	/* Terminator + drain: the caller resets the part right after (semihosting sh_exit), which
	 * would otherwise cut off whatever is still in the UART FIFO — this line included. */
	ove_lxp_console_write("[stack] end\n");
	ove_time_delay_ms(50);
}

static void demo_body(void *arg)
{
	UNUSED(arg);
	(void)ove_lxp_console_init(); /* bring up the program console before any I/O */
	ove_lxp_console_write("=== oveRTOS demo: a native RTOS thread + a Linux program, two-way ===\n");
	/* First line out of the box: identifies the running image against the ELF on
	 * disk, so a stale target cannot be debugged with the wrong symbols. */
	ove_lxp_console_write("[build] " OVE_BUILD_ID "\n");

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	if (linux_rt_scope_start(ove_lxp_console_write) != OVE_OK) {
		ove_lxp_console_write("[rt-scope] FAIL: timer/event/thread setup\n");
		demo_exit(1);
	}
	ove_lxp_console_write("[rt-scope] CH1=D3/PB4 TIM3 1kHz reference; "
		  "CH2=D4/PG7 critical-thread response\n");
#endif

#if defined(CONFIG_OVE_WATCHDOG)
	/* Say why we booted (a watchdog recovery must not look like a spontaneous reboot), then start
	 * the feeder. The monitor is OVE_PRIO_HIGH, so it arms the IWDG and begins feeding immediately,
	 * independent of the setup work below. */
	ove_lxp_console_write("[reset] cause: ");
	ove_lxp_console_write(ove_reset_cause_str(ove_reset_cause()));
	ove_lxp_console_write("\n");
	if (ove_thread_init(&g_wd, &g_wd_storage, "wd", wd_body, NULL, OVE_PRIO_HIGH,
			    sizeof(g_wd_stack), g_wd_stack) != OVE_OK)
		ove_lxp_console_write("[wd] FAIL: monitor thread init; running without a watchdog\n");
#endif

	const void *rootfs_image;
	size_t rootfs_image_size;
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI) || \
	defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500) || \
	defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
	/* The generated layout is shared with the isolation port and board runner. */
	rootfs_image = LXP_EXTERNAL_ROOTFS;
	rootfs_image_size = LXP_EXTERNAL_ROOTFS_MAX;
#else
	rootfs_image = ove_test_rootfs_cpio;
	rootfs_image_size = ove_test_rootfs_cpio_len;
#endif
	ove_lxp_host_config_t host_config = {
		.rootfs_image = rootfs_image,
		.rootfs_image_size = rootfs_image_size,
		.rootfs_storage = g_rootfs,
		.rootfs_capacity = ROOTFS_MAX_FILES,
		.rootfs_name_storage = g_rootfs_names,
		.rootfs_name_capacity = sizeof(g_rootfs_names),
	};
#if defined(CONFIG_OVE_LINUX_NET)
	/* Product topology remains explicit here; the oveRTOS host owns the native
	 * interface storage, bring-up, rollback, and LXP binding. */
	ove_netif_config_t net_config = {.use_dhcp = 0};
	ove_sockaddr_ipv4(&net_config.static_ip, 172, 1, 1, 2, 0);
	ove_sockaddr_ipv4(&net_config.netmask, 255, 255, 255, 0, 0);
	ove_sockaddr_ipv4(&net_config.gateway, 172, 1, 1, 1, 0);
	host_config.netif_config = &net_config;
	host_config.netif_address_wait_ms = 10000u;
#endif
#if defined(CONFIG_OVE_LINUX_NETFS)
	const ove_lxp_netfs_config_t netfs_config = {
		.mountpoint = CONFIG_OVE_LINUX_NETFS_MOUNTPOINT,
		.server_ipv4 = CONFIG_OVE_LINUX_NETFS_SERVER_IP,
		.port = (uint16_t)CONFIG_OVE_LINUX_NETFS_PORT,
		.aname = CONFIG_OVE_LINUX_NETFS_ANAME,
		.uname = "root",
	};
	host_config.netfs_config = &netfs_config;
#endif
	/* The host publishes the rootfs window before parsing, owns native provider
	 * setup, and retains immutable topology/rootfs composition for each run. */
	int rootfs_rc = ove_lxp_host_init_cpio(&g_linux_host, &host_config);
	if (rootfs_rc != LXP_OK) {
		char b[96];
		char *p = put_str(b, "[demo] FAIL: rootfs host init failed rc=");
		p = put_sdec(p, rootfs_rc);
		p = put_str(p, " max_files=");
		p = put_dec(p, ROOTFS_MAX_FILES);
		p = put_str(p, " namebuf=");
		p = put_dec(p, sizeof(g_rootfs_names));
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(b);
		demo_exit(1);
	}

#if defined(CONFIG_OVE_LINUX_NET)
	/* Report the host-owned native interface without exposing its handle. */
	{
		ove_sockaddr_t ip = {0}, gw = {0}, nm = {0};
		if (ove_lxp_host_netif_get_addr(&g_linux_host, &ip, &gw, &nm) == OVE_OK) {
			char b[96];
			char *p = put_str(b, "[demo] eth0 up ip=");
			p = put_dec(p, ip.addr[0]);
			*p++ = '.';
			p = put_dec(p, ip.addr[1]);
			*p++ = '.';
			p = put_dec(p, ip.addr[2]);
			*p++ = '.';
			p = put_dec(p, ip.addr[3]);
			p = put_str(p, " gw=");
			p = put_dec(p, gw.addr[0]);
			*p++ = '.';
			p = put_dec(p, gw.addr[1]);
			*p++ = '.';
			p = put_dec(p, gw.addr[2]);
			*p++ = '.';
			p = put_dec(p, gw.addr[3]);
			*p++ = '\n';
			*p = 0;
			ove_lxp_console_write(b);
		} else {
			ove_lxp_console_write("[demo] eth0 address unavailable after bring-up\n");
		}
	}
#endif

	/* ---- Phase 1: bidirectional round trip through BusyBox `cat` ---------- */
	ove_lxp_console_write("\n-- phase 1: RTOS thread <-> Linux program (bidirectional) --\n");
	/* BELOW the demo task (OVE_PRIO_NORMAL): the worker feeds the readings (before the program
	 * launches) and drains its output (during/after the run). It runs when demo_body blocks — in
	 * the pre-feed wait just below and, once the program is running, in the event-driven
	 * coordinator's event_wait — so both directions co-run without the worker preempting the
	 * coordinator. (The loader's QUADSPI-NOR reads are preemption-safe in their own right — the
	 * coordinator reads the NOR through a non-cacheable bounded MPU region, see
	 * rootfs_window callback — so this priority is about I/O ordering, not protecting the load.) */
	if (ove_thread_init(&g_worker, &g_worker_storage, "rtos-worker", rtos_worker, NULL,
			    OVE_PRIO_LOW, sizeof(g_worker_stack), g_worker_stack) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: ove_thread_init\n");
		demo_exit(1);
	}
	while (!g_feed_ready) /* let the feeder fill the queue before the program reads */
		ove_thread_sleep_ms(1); /* BLOCKING sleep so the lower-priority worker runs: NuttX's
					 * ove_time_delay_ms(1) busy-waits sub-tick (10 ms tick) and never
					 * yields → the OVE_PRIO_LOW worker starves and the demo hangs here
					 * before phase 2.  ove_thread_sleep_ms always usleep()s. */

	const lxp_launch_config_t cfg1 = {
		.write_fn = consume_write,
		.read_fn = feed_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
		.on_guest_exit = on_guest_exit,
		.rt_scope_read = linux_rt_scope_proc_read,
	};
	const char *const cat_argv[] = {"cat", NULL}; /* reads stdin -> writes stdout */
	ove_lxp_console_write("[demo] launching the Linux program (BusyBox cat) to relay the readings...\n");
	int rc1 = ove_lxp_host_run(&g_linux_host, &cfg1, "/bin/busybox", 1, cat_argv);

	g_linux_done = 1;
	while (!g_worker_exited) /* wait for the worker to drain and return */
		ove_thread_sleep_ms(1); /* blocking — must yield to the lower-priority worker (see above) */
	(void)ove_thread_deinit(g_worker);

	int ok = (rc1 >= 0) && (g_round_trip_n == N_READINGS);
	for (int i = 0; ok && i < N_READINGS; i++) {
		char want[16];
		char *p = put_str(want, "reading-");
		p = put_dec(p, (uint32_t)(i + 1));
		*p = 0;
		ok = (strcmp(g_round_trip[i], want) == 0);
	}
	if (!ok) {
		char b[112];
		char *p = put_str(b, "[demo] FAIL: phase-1 round trip mismatch rc=");
		p = put_sdec(p, rc1);
		p = put_str(p, " received=");
		p = put_dec(p, (uint32_t)g_round_trip_n);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(b);
		demo_exit(1);
	}
	ove_lxp_console_write(
		"[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.\n");

#if defined(CONFIG_OVE_LINUX_NET)
	/* A real post-readiness TCP round trip over the same host transport used by
	 * personality sockets and netfs. */
	network_transport_smoke();
#endif

#if defined(CONFIG_OVE_LINUX_FAULTTEST)
	faulttest_maybe_arm(); /* C6: a host task will fault ~4s into phase 2, while the guest runs */
#endif
#if defined(CONFIG_OVE_LINUX_SMASHTEST)
	smashtest_maybe_arm(); /* C9: a host task will smash its stack canary ~4.5s into phase 2 */
#endif

	/* ---- Phase 2: boot userspace or run the hard-float context regression - */
#if defined(CONFIG_OVE_LINUX_GUEST_FP_SELFTEST)
	ove_lxp_console_write("\n-- phase 2: hard-float guest context self-test --\n");
#else
	ove_lxp_console_write("\n-- phase 2: booting uClinux (BusyBox init -> rcS -> login shell;"
		  " run commands, `poweroff` to halt) --\n");
#endif
	lxp_launch_config_t cfg2 = {
		.on_enosys = on_enosys,
		.on_guest_exit = on_guest_exit,
		.rt_scope_read = linux_rt_scope_proc_read,
	};
	ove_lxp_console_bind(&cfg2);
	int rc2;
#if LXP_ENABLE_LATENCY
	/* Start the monitor here, not before phase 1: lxp_run() resets the coordinator's counters
	 * at entry and it is called once per phase, so a monitor spanning both phases would report
	 * host lateness over a window the coordinator's own rows do not cover. Both now measure
	 * exactly the phase-2 run. */
	if (ove_thread_init(&g_mon, &g_mon_storage, "lat-mon", mon_body, NULL, OVE_PRIO_HIGH,
			    sizeof(g_mon_stack), g_mon_stack) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: latency monitor thread init\n");
		demo_exit(1);
	}
#endif
#if defined(CONFIG_OVE_LINUX_GUEST_FP_SELFTEST)
	const char *const fp_argv[] = {"fpcheck", NULL};
	rc2 = ove_lxp_host_run(&g_linux_host, &cfg2, "/usr/bin/fpcheck", 1, fp_argv);
	ove_lxp_console_write("\n=== interop demo done (hard-float self-test exited) ===\n");
#else
	/* PID 1 = BusyBox init: reads /etc/inittab, runs sysinit + rcS, then respawns
	 * a login shell on the console. */
	const char *const init_argv[] = {"init", NULL};
	rc2 = ove_lxp_host_run(&g_linux_host, &cfg2, "/bin/busybox", 1, init_argv);
	ove_lxp_console_write("\n=== interop demo done (uClinux halted) ===\n");
#endif
#if LXP_ENABLE_LATENCY
	g_mon_stop = 1;
	while (!g_mon_exited) /* it may be mid-sleep; let it observe the stop and leave */
		ove_thread_sleep_ms(1);
	(void)ove_thread_deinit(g_mon);
	lat_report(); /* only after the monitor is stopped: the counters are read unlocked */
#endif
	stack_audit(); /* R9/C3: worst-case stack + heap usage, now that the workload has run */
	demo_exit(rc2 >= 0 ? 0 : 1);
}

void ove_main(void)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	/* FreeRTOS: the scheduler starts in ove_run(); run the demo in a task. */
	if (ove_thread_init(&g_demo, &g_demo_storage, "demo", demo_body, NULL, OVE_PRIO_NORMAL,
			    sizeof(g_demo_stack), g_demo_stack) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: demo thread init\n");
		demo_exit(1);
	}
	ove_run(); /* ove_thread_start_scheduler() — never returns */
#else
	/* Zephyr: ove_main() already runs as a thread with the scheduler up. */
	demo_body(NULL);
#endif
}
