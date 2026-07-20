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
 * scheduler traffic. Phase 2 supplies a non-blocking UART readiness probe so an
 * empty console parks the guest instead of blocking the coordinator.
 */

#include <string.h>

#include "ove/thread.h"
#include "ove/time.h"

#include "ove/app.h"
#include "lxp/lxp_config.h" /* LXP_NSLOT (the latency report walks the slots) */
#include "lxp/lxp_latency.h"
#include "lxp/lxp_run.h"
#include "lxp/lxp_syscall.h"
#if defined(CONFIG_OVE_LINUX_NET)
#include "ove/net.h"	    /* bring eth0 up so the personality's sockets can reach the LAN */
#if defined(CONFIG_OVE_LINUX_DEV_DMA2D)
#include "ove/hal/hal_dma2d.h"
#endif
#include "lxp/lxp_net.h" /* lxp_sock_set_netif — the SIOC* ioctl target */
#endif
#if defined(CONFIG_OVE_LINUX_NETFS)
#include "lxp/lxp_netfs.h" /* lxp_netfs_mount_config — the static /mnt/pi mount */
#endif
#if defined(CONFIG_OVE_WATCHDOG)
#include "ove/watchdog.h" /* host-owned IWDG feed */
#include "ove/reset.h"	  /* why the last reset happened (watchdog recovery is visible) */
#endif

#include "ove_config.h" /* CONFIG_OVE_RTOS_FREERTOS — selects the app lifecycle below */

/* Generated per build by 'ove build'; absent when this app is compiled directly
 * from CMake, which is why the fallback has to say so rather than lie. */
#if defined(__has_include)
#if __has_include("ove_build_id.h")
#include "ove_build_id.h"
#endif
#endif
#ifndef OVE_BUILD_ID
#define OVE_BUILD_ID "unknown (built outside 'ove build')"
#endif

#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
/* The rootfs.cpio is programmed into the on-board QSPI NOR, memory-mapped at
 * 0x90000000 (bsp_qspi_init brings up QUADSPI before we parse it), freeing the
 * internal flash for the firmware. The length is an upper bound — the CPIO parse
 * stops at the TRAILER!!! record before the erased tail. */
#define LXP_QSPI_ROOTFS ((const uint8_t *)0x90000000u)
#define LXP_QSPI_ROOTFS_MAX (16u * 1024u * 1024u)
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
/* an500 (QEMU): the rootfs.cpio is XIP'd from PSRAM @ 0x60000000, where QEMU's `-device
 * loader` places it at reset (see qemu-run.sh) — NOT embedded in the ELF, so it never costs
 * the 4 MB internal FLASH. The length is an upper bound (the 12 MiB PSRAM rootfs window); the
 * CPIO parse stops at the TRAILER!!! record. */
#define LXP_PSRAM_ROOTFS ((const uint8_t *)0x60000000u)
#define LXP_PSRAM_ROOTFS_MAX (12u * 1024u * 1024u)
#else
#include "loader_rootfs_image.h" /* ove_test_rootfs_cpio[], _len — a real Buildroot rootfs */
#endif

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/* ---- the Linux-personality port binding -----------------------------------
 * The module's lxp_run() is the port entry: it takes the engine vtable plus the
 * net/display ports. oveRTOS supplies the engine (g_lxp_host_engine, from the
 * compiled freertos/zephyr/nuttx seam) and pre-wires the net/display ports via
 * the backends/common adapter globals (g_lxp_net_ops / g_lxp_disp_ops), so we pass
 * NULL for those (lxp_run keeps a pre-set global) and NULL config (geometry stays
 * at the board default). */
extern const lxp_os_ops_t g_lxp_host_engine;

static int app_lxp_run(const lxp_run_config_t *cfg, const char *path, int argc,
		       const char *const argv[])
{
	return lxp_run(&g_lxp_host_engine, NULL, NULL, NULL, cfg, path, argc, argv);
}

/* ---- the personality console (program stdin/stdout + program exit) --------- */
/* Driven from the privileged coordinator task; must be NON-BLOCKING-pollable so
 * interactive top's 'q' quit works (a finite poll reports readiness instead of blocking the
 * whole CPU the way semihosting SYS_READC would). */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Real STM32F746 hardware: serial_wrapper.c owns USART1 and its IRQ-filled RX buffer. */
extern void serial_poll_begin(void);
extern int serial_poll_rx_ready(void);
extern int serial_poll_getc(void);
extern void serial_poll_putc(char c);
#if defined(CONFIG_OVE_RTOS_FREERTOS)
extern void serial_write(const unsigned char *data, unsigned int length);
#endif

static void uart_init(void)
{
	serial_poll_begin();
}
static void sh_writec(char c)
{
	serial_poll_putc(c);
}
static int uart_rx_ready(void)
{
	return serial_poll_rx_ready();
}
static int sh_readc(void)
{
	while (!serial_poll_rx_ready()) { /* block until a keystroke arrives */
	}
	return serial_poll_getc();
}
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
/* QEMU an500/an521: the engines use UART0 for their own console; the program console rides the
 * CMSDK UART1 (`-serial none -serial stdio` routes UART1 to stdio). SYS_EXIT_EXTENDED via ARM
 * semihosting gives QEMU a clean exit. The MMIO is reachable from the privileged context. */
static long semihost(unsigned long op, void *arg)
{
	register unsigned long r0 __asm__("r0") = op;
	register void *r1 __asm__("r1") = arg;
	__asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
	return (long)r0;
}
#if defined(CONFIG_OVE_RTOS_ZEPHYR)
#define OVE_UART1_BASE 0x50201000u /* AN521 UART1 (secure peripheral region) */
#else
#define OVE_UART1_BASE 0x40005000u /* AN500 UART1 */
#endif
#define OVE_UART_REG(off) (*(volatile unsigned int *)(OVE_UART1_BASE + (off)))
/* CMSDK regs: DATA=0x00, STATE=0x04 (b0 TX-full, b1 RX-valid), CTRL=0x08, BAUDDIV=0x10. */
static void uart_init(void)
{
	OVE_UART_REG(0x10) = 16;  /* BAUDDIV >= 16 required to operate */
	OVE_UART_REG(0x08) = 0x3; /* TX enable | RX enable */
}
static void sh_writec(char c)
{
	while (OVE_UART_REG(0x04) & 1u) { /* spin while the TX buffer is full */
	}
	OVE_UART_REG(0x00) = (unsigned char)c;
}
static int uart_rx_ready(void)
{
	return (OVE_UART_REG(0x04) & 2u) ? 1 : 0; /* RX-valid bit */
}
static int sh_readc(void)
{
	while (!uart_rx_ready()) { /* block until a keystroke arrives */
	}
	return (int)(OVE_UART_REG(0x00) & 0xffu);
}
static void sh_exit(unsigned int code)
{
	unsigned long block[2] = {0x20026u /* ADP_Stopped_ApplicationExit */, code};
	semihost(0x20 /* SYS_EXIT_EXTENDED */, block);
	for (;;) {
	}
}
#endif

/* stdout string helper shared by both console backends. */
static void sh_write0(const char *s)
{
	for (; *s; s++)
		sh_writec(*s);
}

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
		sh_write0(b2);
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
			sh_write0(line);
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

/* ---- phase 2 callbacks: a live console for the interactive shell ------------ */
/* stdin: one real keystroke at a time (the shell's line editor reads char-by-char). */
static long console_read(void *ctx, int fd, void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
	if (len == 0)
		return 0;
	int c = sh_readc();
	if (c < 0)
		return 0; /* host EOF -> the shell exits */
	if (c == '\n')
		c = '\r'; /* normalize Enter to CR: the shell's raw line editor expects it */
	*(char *)buf = (char)c;
	return 1;
}

/* stdout: use the task-context board writer on hardware. It serializes output,
 * translates newlines, and has one total deadline for the whole callback. */
static long console_write(void *ctx, int fd, const void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && defined(CONFIG_OVE_RTOS_FREERTOS)
	serial_write((const unsigned char *)buf, (unsigned int)len);
#else
	const char *p = (const char *)buf;
	for (size_t i = 0; i < len; i++) {
		if (p[i] == '\n')
			sh_writec('\r');
		sh_writec(p[i]);
	}
#endif
	return (long)len;
}

static void on_enosys(long nr)
{
	char b[40];
	char *p = put_str(b, "[demo] unimplemented syscall nr=");
	p = put_dec(p, (uint32_t)nr);
	*p++ = '\n';
	*p = 0;
	sh_write0(b);
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
	sh_write0(b);
}

/* ---- rootfs (parsed from the embedded Buildroot CPIO) ---------------------- */
#define ROOTFS_MAX_FILES 512
static lxp_file_t g_rootfs[ROOTFS_MAX_FILES];
static char g_rootfs_names[16 * 1024];
static int g_rootfs_n;

/* The engine-agnostic demo. On FreeRTOS the scheduler starts inside ove_run(), so
 * this must run in a task; on Zephyr ove_main() is already a running thread and
 * calls it inline. ove_main() (below) wires the per-engine lifecycle. */
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
		sh_write0("[wd] selftest: recovered from the watchdog reset; not re-tripping\n");
		return;
	}
	sh_write0("[wd] selftest: starving the coordinator (scheduler stays live);"
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
		sh_write0("[wd] FAIL: could not arm IWDG; running without a watchdog\n");
		return;
	}
	sh_write0("[wd] IWDG armed: 2000ms timeout, fed every 250ms while the host stays live\n");

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
	ove_thread_sleep_ms(4000); /* let phase 2 bring the guest up, so g_lxp_active is set */
	sh_write0("[c6] faulting a privileged host task (udf) while a guest runs;"
		  " expect HOST FAULT + watchdog reset\n");
	__asm volatile("udf #0"); /* UsageFault in host context -> seam declines -> ove_lnx_host_fatal */
	for (;;) { /* unreachable */
	}
}

static void faulttest_maybe_arm(void)
{
	if (ove_reset_cause() == OVE_RESET_WATCHDOG) {
		sh_write0("[c6] recovered from the host-fault test; not re-arming\n");
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
	sh_write0("[c9] smashing a host stack buffer; expect STACK SMASH + watchdog reset\n");
	smash_host_stack(); /* returns through a smashed canary -> __stack_chk_fail */
	for (;;) { /* unreachable */
	}
}

static void smashtest_maybe_arm(void)
{
	if (ove_reset_cause() == OVE_RESET_WATCHDOG) {
		sh_write0("[c9] recovered from the smash test; not re-arming\n");
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
	sh_write0(line);
}

static void lat_report(void)
{
	sh_write0("\n=== latency (measurement build; no threshold is enforced) ===\n"
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
	sh_write0("[lat] end\n");
	ove_time_delay_ms(50);
}
#endif /* LXP_ENABLE_LATENCY */

/* Non-blocking console-readiness probe for the personality's poll(2) (interactive
 * top's 'q' quit): true when a UART1 RX byte is waiting. */
static int console_poll(void *ctx)
{
	UNUSED(ctx);
	return uart_rx_ready();
}

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
	sh_write0(line);
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
	sh_write0(line);
}
#endif /* !CONFIG_OVE_RTOS_FREERTOS */

static void stack_audit(void)
{
	sh_write0("\n=== stack high-water audit (deepest usage this run) ===\n");
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
	/* Guest-slot tramp stack (TRAMP_STACK_WORDS=256 words = 1024 B in the seam): worst across all
	 * slots this run. Only the entry prologue — the guest runs on its own arena stack. */
	extern size_t ove_lnx_slot_stack_hwm(void);
	audit_stack_line("guest-slot(tramp)", ove_lnx_slot_stack_hwm(), 1024u);
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
		sh_write0(line);
	}
	/* Terminator + drain: the caller resets the part right after (semihosting sh_exit), which
	 * would otherwise cut off whatever is still in the UART FIFO — this line included. */
	sh_write0("[stack] end\n");
	ove_time_delay_ms(50);
}

static void demo_body(void *arg)
{
	UNUSED(arg);
	uart_init(); /* bring up the UART1 program console before any I/O */
	sh_write0("=== oveRTOS demo: a native RTOS thread + a Linux program, two-way ===\n");
	/* First line out of the box: identifies the running image against the ELF on
	 * disk, so a stale target cannot be debugged with the wrong symbols. */
	sh_write0("[build] " OVE_BUILD_ID "\n");

#if defined(CONFIG_OVE_WATCHDOG)
	/* Say why we booted (a watchdog recovery must not look like a spontaneous reboot), then start
	 * the feeder. The monitor is OVE_PRIO_HIGH, so it arms the IWDG and begins feeding immediately,
	 * independent of the setup work below. */
	sh_write0("[reset] cause: ");
	sh_write0(ove_reset_cause_str(ove_reset_cause()));
	sh_write0("\n");
	if (ove_thread_init(&g_wd, &g_wd_storage, "wd", wd_body, NULL, OVE_PRIO_HIGH,
			    sizeof(g_wd_stack), g_wd_stack) != OVE_OK)
		sh_write0("[wd] FAIL: monitor thread init; running without a watchdog\n");
#endif

#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	/* The rootfs is XIP'd from the memory-mapped QUADSPI NOR.  Declare that window to the
	 * personality BEFORE the first read of it (the CPIO parse just below): on the STM32F746 this
	 * installs a bounded, non-cacheable MPU region for this coordinator task so the M7 D-cache
	 * neither bursts nor speculates into the QUADSPI (a no-op on targets without that hazard). */
	lxp_rootfs_window(LXP_QSPI_ROOTFS, LXP_QSPI_ROOTFS_MAX);
	g_rootfs_n = lxp_cpio_to_rootfs(LXP_QSPI_ROOTFS, LXP_QSPI_ROOTFS_MAX, g_rootfs,
					    ROOTFS_MAX_FILES, g_rootfs_names, sizeof(g_rootfs_names));
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
	/* PSRAM is ordinary RAM in QEMU (no D-cache / external-NOR hazard), so — unlike the STM32
	 * QSPI case — no lxp_rootfs_window MPU shim is needed; the engine-common weak no-op stands. */
	g_rootfs_n = lxp_cpio_to_rootfs(LXP_PSRAM_ROOTFS, LXP_PSRAM_ROOTFS_MAX, g_rootfs,
					    ROOTFS_MAX_FILES, g_rootfs_names, sizeof(g_rootfs_names));
#else
	g_rootfs_n = lxp_cpio_to_rootfs(ove_test_rootfs_cpio, ove_test_rootfs_cpio_len,
					    g_rootfs, ROOTFS_MAX_FILES, g_rootfs_names,
					    sizeof(g_rootfs_names));
#endif
	if (g_rootfs_n <= 0) {
		char b[96];
		char *p = put_str(b, "[demo] FAIL: rootfs CPIO parse failed n=");
		p = put_dec(p, (uint32_t)g_rootfs_n);
		p = put_str(p, " max_files=");
		p = put_dec(p, ROOTFS_MAX_FILES);
		p = put_str(p, " namebuf=");
		p = put_dec(p, sizeof(g_rootfs_names));
		*p++ = '\n';
		*p = 0;
		sh_write0(b);
		sh_exit(1);
	}

#if defined(CONFIG_OVE_LINUX_NET)
	/* Bring eth0 up so the personality's socket layer (FD_SOCKET -> ove_net -> lwIP
	 * + the STM32 LAN8742 driver) can reach the LAN. Static IP (see below); the
	 * configured address is printed so a headless run reports the link came up. */
	{
		static ove_netif_storage_t s_netif_storage;
		ove_netif_t nif = NULL;
		/* Static IP: the board's eth0 is a point-to-point link to the test host
		 * (a Raspberry Pi at 172.1.1.1/24); no DHCP on that link. */
		ove_netif_config_t netcfg = {.use_dhcp = 0};
		ove_sockaddr_ipv4(&netcfg.static_ip, 172, 1, 1, 2, 0);
		ove_sockaddr_ipv4(&netcfg.netmask, 255, 255, 255, 0, 0);
		ove_sockaddr_ipv4(&netcfg.gateway, 172, 1, 1, 1, 0);
		if (ove_netif_init(&nif, &s_netif_storage) == OVE_OK &&
		    ove_netif_up(nif, &netcfg) == OVE_OK) {
			/* Register the interface so the personality's SIOC* ioctls (ifconfig/route)
			 * operate on it. */
			lxp_sock_set_netif(nif);
			ove_sockaddr_t ip = {0}, gw = {0}, nm = {0};
			for (int i = 0; i < 200; i++) { /* wait for the interface to report its address */
				ove_netif_get_addr(nif, &ip, &gw, &nm);
				if (ip.addr[0] | ip.addr[1] | ip.addr[2] | ip.addr[3])
					break;
				ove_thread_sleep_ms(50);
			}
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
			sh_write0(b);

#if defined(CONFIG_OVE_LINUX_DEV_DMA2D)
			/* Coordinator-side DMA2D proof: a HW fill + read-back (register path +
			 * D-cache coherency) before any guest touches /dev/dma2d. Boards with
			 * only the weak fallback report "no HW backend". */
			{
				int di = ove_hal_dma2d_init();
				int dr = (di == OVE_OK) ? ove_hal_dma2d_selftest() : di;
				sh_write0(dr == OVE_OK ? "[dma2d] selftest: OK\n"
					  : dr == OVE_ERR_NOT_SUPPORTED
						  ? "[dma2d] selftest: no HW backend (sw fallback)\n"
						  : "[dma2d] selftest: FAIL\n");
			}
#endif

			/* On-silicon transport smoke: connect to the test host's sshd and read
			 * its banner — a real TCP round-trip over the STM32 Ethernet (ove_net ->
			 * lwIP -> LAN8742). The personality's FD_SOCKET bridge over this same
			 * transport is covered by the host loopback cmocka test. */
			static ove_socket_storage_t s_sk;
			ove_socket_t sk = NULL;
			if (ove_socket_open(&sk, &s_sk, OVE_AF_INET, OVE_SOCK_STREAM) == OVE_OK) {
				ove_sockaddr_t peer;
				ove_sockaddr_ipv4(&peer, 172, 1, 1, 1, 22);
				int cr = ove_socket_connect(sk, &peer, OVE_SEC(5));
				if (cr == OVE_OK) {
					char rb[80];
					size_t got = 0;
					if (ove_socket_recv(sk, rb, sizeof(rb) - 1, &got, OVE_SEC(5)) ==
						    OVE_OK &&
					    got > 0) {
						for (size_t i = 0; i < got; i++)
							if (rb[i] == '\r' || rb[i] == '\n')
								rb[i] = 0;
						rb[got < sizeof(rb) ? got : sizeof(rb) - 1] = 0;
						char b2[128];
						char *q = put_str(b2, "[demo] socket smoke OK <- 172.1.1.1:22: ");
						q = put_str(q, rb);
						*q++ = '\n';
						*q = 0;
						sh_write0(b2);
					} else {
						sh_write0("[demo] socket smoke: connected, no banner\n");
					}
				} else {
					char b2[64];
					char *q = put_str(b2, "[demo] socket smoke: connect failed rc=-");
					q = put_dec(q, (uint32_t)(-cr));
					*q++ = '\n';
					*q = 0;
					sh_write0(b2);
				}
				ove_socket_close(sk);
			}
		} else {
			sh_write0("[demo] eth0 bring-up FAILED\n");
		}
	}
#endif
#if defined(CONFIG_OVE_LINUX_NETFS)
	/* Configure the static remote-fs mount (/mnt/pi -> the Pi's 9P/diod export). The 9P
	 * handshake happens later inside lxp_run's coordinator (lxp_netfs_init). */
	{
		uint8_t ip[4] = {0, 0, 0, 0};
		int oct = 0, v = 0;
		for (const char *c = CONFIG_OVE_LINUX_NETFS_SERVER_IP;; c++) {
			if (*c >= '0' && *c <= '9') {
				v = v * 10 + (*c - '0');
			} else {
				if (oct < 4)
					ip[oct] = (uint8_t)v;
				oct++;
				v = 0;
				if (!*c)
					break;
			}
		}
		lxp_netfs_mount_config(CONFIG_OVE_LINUX_NETFS_MOUNTPOINT, ip,
					   (uint16_t)CONFIG_OVE_LINUX_NETFS_PORT,
					   CONFIG_OVE_LINUX_NETFS_ANAME, "root");
	}
#endif

	/* ---- Phase 1: bidirectional round trip through BusyBox `cat` ---------- */
	sh_write0("\n-- phase 1: RTOS thread <-> Linux program (bidirectional) --\n");
	/* BELOW the demo task (OVE_PRIO_NORMAL): the worker feeds the readings (before the program
	 * launches) and drains its output (during/after the run). It runs when demo_body blocks — in
	 * the pre-feed wait just below and, once the program is running, in the event-driven
	 * coordinator's event_wait — so both directions co-run without the worker preempting the
	 * coordinator. (The loader's QUADSPI-NOR reads are preemption-safe in their own right — the
	 * coordinator reads the NOR through a non-cacheable bounded MPU region, see
	 * lxp_rootfs_window — so this priority is about I/O ordering, not protecting the load.) */
	if (ove_thread_init(&g_worker, &g_worker_storage, "rtos-worker", rtos_worker, NULL,
			    OVE_PRIO_LOW, sizeof(g_worker_stack), g_worker_stack) != OVE_OK) {
		sh_write0("[demo] FAIL: ove_thread_init\n");
		sh_exit(1);
	}
	while (!g_feed_ready) /* let the feeder fill the queue before the program reads */
		ove_thread_sleep_ms(1); /* BLOCKING sleep so the lower-priority worker runs: NuttX's
					 * ove_time_delay_ms(1) busy-waits sub-tick (10 ms tick) and never
					 * yields → the OVE_PRIO_LOW worker starves and the demo hangs here
					 * before phase 2.  ove_thread_sleep_ms always usleep()s. */

	const lxp_run_config_t cfg1 = {
		.rootfs = g_rootfs,
		.rootfs_count = g_rootfs_n,
		.write_fn = consume_write,
		.read_fn = feed_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
		.on_guest_exit = on_guest_exit,
	};
	const char *const cat_argv[] = {"cat", NULL}; /* reads stdin -> writes stdout */
	sh_write0("[demo] launching the Linux program (BusyBox cat) to relay the readings...\n");
	int rc1 = app_lxp_run(&cfg1, "/bin/busybox", 1, cat_argv);

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
		sh_write0("[demo] FAIL: phase-1 round trip mismatch\n");
		sh_exit(1);
	}
	sh_write0(
		"[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.\n");

#if defined(CONFIG_OVE_LINUX_FAULTTEST)
	faulttest_maybe_arm(); /* C6: a host task will fault ~4s into phase 2, while the guest runs */
#endif
#if defined(CONFIG_OVE_LINUX_SMASHTEST)
	smashtest_maybe_arm(); /* C9: a host task will smash its stack canary ~4.5s into phase 2 */
#endif

	/* ---- Phase 2: boot userspace or run the hard-float context regression - */
#if defined(CONFIG_OVE_LINUX_GUEST_FP_SELFTEST)
	sh_write0("\n-- phase 2: hard-float guest context self-test --\n");
#else
	sh_write0("\n-- phase 2: booting uClinux (BusyBox init -> rcS -> login shell;"
		  " run commands, `poweroff` to halt) --\n");
#endif
	const lxp_run_config_t cfg2 = {
		.rootfs = g_rootfs,
		.rootfs_count = g_rootfs_n,
		.write_fn = console_write,
		.read_fn = console_read,
		.console_poll = console_poll,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
		.on_guest_exit = on_guest_exit,
	};
	int rc2;
#if LXP_ENABLE_LATENCY
	/* Start the monitor here, not before phase 1: lxp_run() resets the coordinator's counters
	 * at entry and it is called once per phase, so a monitor spanning both phases would report
	 * host lateness over a window the coordinator's own rows do not cover. Both now measure
	 * exactly the phase-2 run. */
	if (ove_thread_init(&g_mon, &g_mon_storage, "lat-mon", mon_body, NULL, OVE_PRIO_HIGH,
			    sizeof(g_mon_stack), g_mon_stack) != OVE_OK) {
		sh_write0("[demo] FAIL: latency monitor thread init\n");
		sh_exit(1);
	}
#endif
#if defined(CONFIG_OVE_LINUX_GUEST_FP_SELFTEST)
	const char *const fp_argv[] = {"fpcheck", NULL};
	rc2 = app_lxp_run(&cfg2, "/usr/bin/fpcheck", 1, fp_argv);
	sh_write0("\n=== interop demo done (hard-float self-test exited) ===\n");
#else
	/* PID 1 = BusyBox init: reads /etc/inittab, runs sysinit + rcS, then respawns
	 * a login shell on the console. */
	const char *const init_argv[] = {"init", NULL};
	rc2 = app_lxp_run(&cfg2, "/bin/busybox", 1, init_argv);
	sh_write0("\n=== interop demo done (uClinux halted) ===\n");
#endif
#if LXP_ENABLE_LATENCY
	g_mon_stop = 1;
	while (!g_mon_exited) /* it may be mid-sleep; let it observe the stop and leave */
		ove_thread_sleep_ms(1);
	(void)ove_thread_deinit(g_mon);
	lat_report(); /* only after the monitor is stopped: the counters are read unlocked */
#endif
	stack_audit(); /* R9/C3: worst-case stack + heap usage, now that the workload has run */
	sh_exit(rc2 >= 0 ? 0 : 1);
}

void ove_main(void)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	/* FreeRTOS: the scheduler starts in ove_run(); run the demo in a task. */
	if (ove_thread_init(&g_demo, &g_demo_storage, "demo", demo_body, NULL, OVE_PRIO_NORMAL,
			    sizeof(g_demo_stack), g_demo_stack) != OVE_OK) {
		sh_write0("[demo] FAIL: demo thread init\n");
		sh_exit(1);
	}
	ove_run(); /* ove_thread_start_scheduler() — never returns */
#else
	/* Zephyr: ove_main() already runs as a thread with the scheduler up. */
	demo_body(NULL);
#endif
}
