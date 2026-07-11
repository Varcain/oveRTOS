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
 * The personality's I/O callbacks run in the svc-trap (exception/handler) context.
 * That context is above configMAX_SYSCALL_INTERRUPT_PRIORITY, where the FreeRTOS
 * FromISR queue APIs are undefined (their list ops race the scheduler — with the
 * D-cache on the timing shift makes that corrupt a list and hang vListInsert). So
 * phase 1 crosses the boundary with plain arrays + a published index, no RTOS list
 * op from the handler. Phase 1 stages the feed array before launching, so the
 * program never sees a premature EOF. (The console transport is ARM semihosting /
 * a non-blocking UART — an architecture facility, not an RTOS primitive.)
 */

#include <string.h>

#include "ove/thread.h"
#include "ove/time.h"

#include "ove/app.h"
#include "ove/linux/run.h"
#include "ove/linux/syscall.h"
#if defined(CONFIG_OVE_LINUX_NET)
#include "ove/net.h"	    /* bring eth0 up so the personality's sockets can reach the LAN */
#include "ove/linux/net.h" /* lxp_sock_set_netif — the SIOC* ioctl target */
#endif
#if defined(CONFIG_OVE_LINUX_NETFS)
#include "ove/linux/netfs.h" /* lxp_netfs_mount_config — the static /mnt/pi mount */
#endif

#include "ove_config.h" /* CONFIG_OVE_RTOS_FREERTOS — selects the app lifecycle below */

#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
/* The rootfs.cpio is programmed into the on-board QSPI NOR, memory-mapped at
 * 0x90000000 (bsp_qspi_init brings up QUADSPI before we parse it), freeing the
 * internal flash for the firmware. The length is an upper bound — the CPIO parse
 * stops at the TRAILER!!! record before the erased tail. */
#define LXP_QSPI_ROOTFS ((const uint8_t *)0x90000000u)
#define LXP_QSPI_ROOTFS_MAX (16u * 1024u * 1024u)
#else
#include "loader_rootfs_image.h" /* ove_test_rootfs_cpio[], _len — a real Buildroot rootfs */
#endif

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/* ---- the personality console (program stdin/stdout + program exit) --------- */
/* Driven from the PRIVILEGED personality context; must be NON-BLOCKING-pollable so
 * interactive top's 'q' quit works (a finite poll reports readiness instead of blocking the
 * whole CPU the way semihosting SYS_READC would). */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Real STM32F746 hardware: poll the board's USART1 directly. The personality reads in the
 * svc-exception context, where the board's IRQ-filled RX buffer cannot be used (the svc masks
 * the prio-2 USART1 IRQ → a blocking read would deadlock), so serial_wrapper.c hands the
 * receiver to polled register access and owns the USART/HAL details. */
extern void serial_poll_begin(void);
extern int serial_poll_rx_ready(void);
extern int serial_poll_getc(void);
extern void serial_poll_putc(char c);

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

/* Phase-1 I/O uses plain arrays, NOT FreeRTOS queues. feed_read/consume_write run in the guest's
 * SVC handler (handler mode, above configMAX_SYSCALL_INTERRUPT_PRIORITY), where the FromISR queue
 * APIs are UNDEFINED — their internal FreeRTOS-list operations race the scheduler and (with the M7
 * D-cache on, which speeds the guest up and shifts the timing) corrupt a list, hanging vListInsert.
 * A pre-staged array served by a published index touches no FreeRTOS list, so it is safe from the
 * handler. The native worker thread + the two-way data flow are preserved. */
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

/* stdout: echo to the host console; translate \n -> \r\n so it reads cleanly. */
static long console_write(void *ctx, int fd, const void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
	const char *p = (const char *)buf;
	for (size_t i = 0; i < len; i++) {
		if (p[i] == '\n')
			sh_writec('\r');
		sh_writec(p[i]);
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
static uint8_t g_demo_stack[4096] __attribute__((aligned(8)));
#endif

/* Non-blocking console-readiness probe for the personality's poll(2) (interactive
 * top's 'q' quit): true when a UART1 RX byte is waiting. */
static int console_poll(void *ctx)
{
	UNUSED(ctx);
	return uart_rx_ready();
}

static void demo_body(void *arg)
{
	UNUSED(arg);
	uart_init(); /* bring up the UART1 program console before any I/O */
	sh_write0("=== oveRTOS demo: a native RTOS thread + a Linux program, two-way ===\n");

#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	/* The rootfs is XIP'd from the memory-mapped QUADSPI NOR.  Declare that window to the
	 * personality BEFORE the first read of it (the CPIO parse just below): on the STM32F746 this
	 * installs a bounded, non-cacheable MPU region for this coordinator task so the M7 D-cache
	 * neither bursts nor speculates into the QUADSPI (a no-op on targets without that hazard). */
	lxp_rootfs_window(LXP_QSPI_ROOTFS, LXP_QSPI_ROOTFS_MAX);
	g_rootfs_n = lxp_cpio_to_rootfs(LXP_QSPI_ROOTFS, LXP_QSPI_ROOTFS_MAX, g_rootfs,
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
	};
	const char *const cat_argv[] = {"cat", NULL}; /* reads stdin -> writes stdout */
	sh_write0("[demo] launching the Linux program (BusyBox cat) to relay the readings...\n");
	int rc1 = lxp_run(&cfg1, "/bin/busybox", 1, cat_argv);

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

	/* ---- Phase 2: boot a full uClinux userspace --------------------------- */
	sh_write0("\n-- phase 2: booting uClinux (BusyBox init -> rcS -> login shell;"
		  " run commands, `poweroff` to halt) --\n");
	const lxp_run_config_t cfg2 = {
		.rootfs = g_rootfs,
		.rootfs_count = g_rootfs_n,
		.write_fn = console_write,
		.read_fn = console_read,
		.console_poll = console_poll,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
	};
	/* PID 1 = BusyBox init: reads /etc/inittab, runs sysinit + rcS, then respawns
	 * a login shell on the console. */
	const char *const init_argv[] = {"init", NULL};
	int rc2 = lxp_run(&cfg2, "/bin/busybox", 1, init_argv);

	sh_write0("\n=== interop demo done (uClinux halted) ===\n");
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
