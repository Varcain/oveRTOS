/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Internal interface between the engine-agnostic Linux-personality run loop +
 * svc dispatch (backends/common/lxp_run.c) and the per-engine seams under
 * backends/zephyr, backends/freertos, backends/nuttx. NOT a public API.
 *
 * The shared core owns the NOMMU process model — the vfork/exec/wait run loop,
 * the syscall-dispatch body, and signal delivery — all written against a uniform
 * register frame and a small per-engine vtable. Each seam supplies only what
 * genuinely differs: the svc-trap mechanism, the program memory (whose placement
 * differs — e.g. Zephyr MPU partitions), and the task spawn/abort.
 */

#ifndef OVE_BACKENDS_COMMON_LXP_RUN_H
#define OVE_BACKENDS_COMMON_LXP_RUN_H

#include <stddef.h>
#include <stdint.h>

#include "lxp/lxp_config.h" /* LXP_PROG_REGION_SIZE / LXP_NREG / LXP_NSLOT / sizing knobs */
#include "lxp/lxp_loader.h"
#include "lxp/lxp_run.h"
#include "lxp/lxp_syscall.h"

/* Program-region / arena / dyn-pool sizes + LXP_NREG / LXP_NSLOT come from
 * lxp_config.h (host-overridable; the oveRTOS build maps them per engine). */


/* A uniform Cortex-M register frame the dispatch reads/writes. The seam populates
 * it from its native exception frame and writes the modified HW registers back.
 * r[0..15] = r0..r15 (r[13]=sp = the program's pre-svc SP, r[14]=lr, r[15]=pc). */
struct lxp_frame {
	uint32_t r[16];
	uint32_t xpsr;
};

/* Parent context captured at a vfork svc, replayed to resume the parent + child. */
struct lxp_resume_ctx {
	uint32_t r4_11[8];
	uint32_t r12;
	uint32_t lr;
	uint32_t sp;
	uint32_t pc;
};

struct lxp_thread_info; /* <ove/thread.h>; the ps/top snapshot fills an array of these */

/* The per-engine operations the shared run loop drives. */
struct lxp_engine {
	/* The engine owns prog_regions[] (its placement differs per engine). */
	uint8_t *(*region)(int ridx);
	/* Spawn slot `sidx` running the freshly-loaded `prog` at (entry, sp). */
	int (*spawn_launch)(int sidx, int ridx, const lxp_flat_t *prog, void *entry, void *sp,
			    void *stack_lo);
	/* Spawn slot `sidx` resuming at the captured context `ctx` with r0 = r0val.
	 * (Per-proc ctx, not the single global — many forks/sleeps/waits can be
	 * outstanding at once under the concurrent model.) */
	void (*spawn_resume)(int sidx, int ridx, const struct lxp_resume_ctx *ctx, long r0val);
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
	 * with attrs (LXP_MAP_NC/WT/DEV). Coordinator thread only (domain/TCB edits
	 * aren't exception-safe). NULL => a device mmap returns -ENODEV, leaving the
	 * write()/pwrite() framebuffer path unaffected. */
	int (*map_device)(int sidx, uintptr_t addr, size_t size, unsigned attrs);

	/* ---- OS-service hooks (host adapter supplies these) --------------------
	 * The personality core reaches these through the module-internal wrappers
	 * (lxp_time_us/ns, lxp_cache_clean/invalidate) so it no longer calls
	 * ove_time_* / ove_thread_list / the host's cache maintenance directly.
	 * These are the seam of the OS-agnostic extraction: a non-oveRTOS host fills
	 * them from its own clock / scheduler / cache primitives. */
	/* Monotonic clock: *out = microseconds / nanoseconds since boot. Required. */
	int (*time_us)(uint64_t *out);
	int (*time_ns)(uint64_t *out);
	/* Host kernel-thread snapshot for the ps/top /proc view. NULL => omitted. */
	int (*thread_list)(struct lxp_thread_info *out, size_t max_count, size_t *actual_count);
	/* Guest-memory cache maintenance (NULL => no-op; a coherent host needs none). */
	void (*cache_clean)(const void *base, size_t len);
	void (*cache_invalidate)(const void *base, size_t len);
	/* Tell the engine where the XIP rootfs image lives (PC discrimination). NULL => no-op. */
	void (*rootfs_window)(const void *base, size_t len);
	/* Staging buffer for fetching a remote (9P) exec image. NULL => no remote exec. */
	uint8_t *(*exec_stage)(size_t *cap);
};

/* ---- shared state (defined in lxp_run.c) ------------------------------- */
extern struct lxp_resume_ctx g_lxp_vfork; /* vfork capture buffer */
extern lxp_proc_t g_lxp_proc[LXP_NSLOT];
extern int g_lxp_used[LXP_NSLOT]; /* slot in use (run loop + seam read) */
extern volatile int g_lxp_active;	  /* a run is in progress (seam trap gate) */
extern volatile int g_lxp_halt;	  /* reboot(2)/poweroff: stop the run loop */
/* The embedded cpio's data span [lo, hi): a dynamic FDPIC proc runs its shared in-place text from
 * here, so a PC-discriminating seam (NuttX) treats a cpio PC as a program svc. NULL pre-run. */
extern const uint8_t *g_lxp_rootfs_lo, *g_lxp_rootfs_hi;

/* Where a parked program spins (in shared .text) until the run loop reaps it. */
void lxp_park_loop(void);

/* The shared svc-dispatch body. Called by the seam's trap with the uniform frame
 * and the running slot's proc; on return the seam writes the frame back. */
void lxp_dispatch(struct lxp_frame *f, lxp_proc_t *proc);

/* The shared run loop. Each engine's public lxp_run() wraps this. */
int lxp_run_common(const struct lxp_engine *eng, const lxp_run_config_t *cfg,
		       const char *path, int argc, const char *const argv[]);

#endif /* OVE_BACKENDS_COMMON_LXP_RUN_H */
