/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Internal interface between the engine-agnostic Linux-personality run loop +
 * svc dispatch (backends/common/ove_lnx_run.c) and the per-engine seams under
 * backends/zephyr, backends/freertos, backends/nuttx. NOT a public API.
 *
 * The shared core owns the NOMMU process model — the vfork/exec/wait run loop,
 * the syscall-dispatch body, and signal delivery — all written against a uniform
 * register frame and a small per-engine vtable. Each seam supplies only what
 * genuinely differs: the svc-trap mechanism, the program memory (whose placement
 * differs — e.g. Zephyr MPU partitions), and the task spawn/abort.
 */

#ifndef OVE_BACKENDS_COMMON_OVE_LNX_RUN_H
#define OVE_BACKENDS_COMMON_OVE_LNX_RUN_H

#include <stddef.h>
#include <stdint.h>

#include "ove_config.h" /* CONFIG_OVE_RTOS_* for the per-engine NREG sizing below */
#include "ove/loader.h"
#include "ove/linux/run.h"
#include "ove/linux/syscall.h"

#define OVE_LNX_PROG_REGION_SIZE 0x80000u /* 512K: featured BusyBox ~324K + arena + stack */
#define OVE_LNX_PROG_ARENA_SIZE 0x18000u  /* 96K program heap */
#define OVE_LNX_DYN_POOL_SIZE 0x80000u /* 512K: a dynamic proc's arena. Holds every loaded .so's RW
					* segment (curl + libmbedtls/x509/crypto + libc = ~5 libs) + the
					* brk/mmap heap. 256K sufficed for BusyBox (one lib), but curl's
					* mbedTLS handshake — TLS I/O buffers + CA-bundle parse — needs more.
					* NREG(6) x (512K region + 512K pool) + fb fits the 8M SDRAM. In
					* PSRAM (Zephyr) / per-engine RAM. */
/* Concurrent process model (Phase D): the run loop coordinates a live process SET,
 * each loaded image in its own region. OVE_LNX_NREG = max images live at once
 * (init + login-shell + a few concurrent jobs); OVE_LNX_NSLOT = NREG + vfork-window
 * slots (a vfork child shares its parent's region until it execs). Per-engine
 * overridable so an521 (PSRAM, roomy) can run more than the an500 (4 MB) engines. */
/* NREG = max program images live at once (init + login shell + concurrent jobs).
 * Per-engine: Zephyr/an521 places the regions in a NOLOAD 16 MB PSRAM region, so it
 * can afford several (8 × 512K = 4 MB PSRAM) — enough for e.g. two background jobs +
 * top. FreeRTOS/NuttX place the regions in external PSRAM (an500 0x60000000, 16 MB) /
 * SDRAM (STM32F746 0xC0000000, 8 MB) — both far larger than the 4 MB internal RAM — so
 * 6 × 768K = 4.5 MB fits comfortably (the old "NREG=6 overflowed RAM" note was the
 * retired .bss placement). NSLOT = NREG + transient vfork-window slots. */
#ifndef OVE_LNX_NREG
#if defined(CONFIG_OVE_RTOS_ZEPHYR)
#define OVE_LNX_NREG 8
#else
/* A pipeline over SSH nests deep: init + getty + inetd + dropbear + shell + the pipeline
 * members. `ls | head` alone needs 7 live regions; 6 exhausts the pool at the 2nd stage. On the
 * STM32F746 the 512 KB-aligned (MPU-required) 1 MB regions plus the 255 KB LCD framebuffer + ETH
 * bounce leave room for only 6 in the 8 MB SDRAM, so a deep pipeline over SSH runs out — but it
 * now fails the fork cleanly (-ENOMEM) instead of corrupting a parent (see vfork_snapshot). */
#define OVE_LNX_NREG 6
#endif
#endif
#ifndef OVE_LNX_NSLOT
#define OVE_LNX_NSLOT (OVE_LNX_NREG + 4)
#endif

/* A uniform Cortex-M register frame the dispatch reads/writes. The seam populates
 * it from its native exception frame and writes the modified HW registers back.
 * r[0..15] = r0..r15 (r[13]=sp = the program's pre-svc SP, r[14]=lr, r[15]=pc). */
struct ove_lnx_frame {
	uint32_t r[16];
	uint32_t xpsr;
};

/* Parent context captured at a vfork svc, replayed to resume the parent + child. */
struct ove_lnx_resume_ctx {
	uint32_t r4_11[8];
	uint32_t r12;
	uint32_t lr;
	uint32_t sp;
	uint32_t pc;
};

/* The per-engine operations the shared run loop drives. */
struct ove_lnx_engine {
	/* The engine owns prog_regions[] (its placement differs per engine). */
	uint8_t *(*region)(int ridx);
	/* Spawn slot `sidx` running the freshly-loaded `prog` at (entry, sp). */
	int (*spawn_launch)(int sidx, int ridx, const ove_flat_t *prog, void *entry, void *sp,
			    void *stack_lo);
	/* Spawn slot `sidx` resuming at the captured context `ctx` with r0 = r0val.
	 * (Per-proc ctx, not the single global — many forks/sleeps/waits can be
	 * outstanding at once under the concurrent model.) */
	void (*spawn_resume)(int sidx, int ridx, const struct ove_lnx_resume_ctx *ctx, long r0val);
	/* Abort (delete) slot `sidx`'s task. */
	void (*abort_slot)(int sidx);
	/* Sleep the run-loop task for `ms` milliseconds. */
	void (*sleep_ms)(unsigned ms);
	/* Coordinator critical section: mask the program svc EXCEPTION (NOT just thread
	 * preemption) so a program's syscall can't preempt the coordinator mid-edit of
	 * the shared proc table. irq_lock / taskENTER_CRITICAL / enter_critical_section. */
	void (*crit_enter)(void);
	void (*crit_exit)(void);
	/* Event wakeup: the dispatch posts when a program parks (fork/exec/exit/sleep/
	 * wait); the coordinator blocks in event_wait instead of busy-polling — so it
	 * doesn't preempt running programs every tick (which would reset their RTOS
	 * time-slice and let a CPU-bound background job starve the foreground). The wait
	 * also times out (ms) for sleeper deadlines + the ps/top snapshot refresh. */
	void (*event_post)(void);
	void (*event_wait)(unsigned ms);
	/* FDPIC dynamic linking: a per-region scratch pool the dynamic arena lives in — ld.so
	 * mmaps libc.so (~500K) from it, far past the in-region 96K arena. NULL if the engine
	 * has no room (dynamic execs then fail to launch; static FDPIC unaffected). Returns
	 * region `ridx`'s slice + its size. (an500: PSRAM @ 0x60000000.) */
	uint8_t *(*dyn_pool)(int ridx, size_t *size);
	/* Device mmap (Phase P3): map [addr, addr+size) RW into slot sidx's program view
	 * with attrs (OVE_LNX_MAP_NC/WT/DEV). Coordinator thread only (domain/TCB edits
	 * aren't exception-safe). NULL => a device mmap returns -ENODEV, leaving the
	 * write()/pwrite() framebuffer path unaffected. */
	int (*map_device)(int sidx, uintptr_t addr, size_t size, unsigned attrs);
};

/* ---- shared state (defined in ove_lnx_run.c) ------------------------------- */
extern struct ove_lnx_resume_ctx g_ove_lnx_vfork; /* vfork capture buffer */
extern ove_lnx_proc_t g_ove_lnx_proc[OVE_LNX_NSLOT];
extern int g_ove_lnx_used[OVE_LNX_NSLOT]; /* slot in use (run loop + seam read) */
extern volatile int g_ove_lnx_active;	  /* a run is in progress (seam trap gate) */
extern volatile int g_ove_lnx_halt;	  /* reboot(2)/poweroff: stop the run loop */
/* The embedded cpio's data span [lo, hi): a dynamic FDPIC proc runs its shared in-place text from
 * here, so a PC-discriminating seam (NuttX) treats a cpio PC as a program svc. NULL pre-run. */
extern const uint8_t *g_ove_lnx_rootfs_lo, *g_ove_lnx_rootfs_hi;

/* Where a parked program spins (in shared .text) until the run loop reaps it. */
void ove_lnx_park_loop(void);

/* The shared svc-dispatch body. Called by the seam's trap with the uniform frame
 * and the running slot's proc; on return the seam writes the frame back. */
void ove_lnx_dispatch(struct ove_lnx_frame *f, ove_lnx_proc_t *proc);

/* The shared run loop. Each engine's public ove_lnx_run() wraps this. */
int ove_lnx_run_common(const struct ove_lnx_engine *eng, const ove_lnx_run_config_t *cfg,
		       const char *path, int argc, const char *const argv[]);

#endif /* OVE_BACKENDS_COMMON_OVE_LNX_RUN_H */
