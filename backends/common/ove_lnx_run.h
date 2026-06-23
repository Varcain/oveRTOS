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

#include "ove/loader.h"
#include "ove/linux/run.h"
#include "ove/linux/syscall.h"

#define OVE_LNX_PROG_REGION_SIZE 0x60000u /* 384K: BusyBox ~129K + arena + stack */
#define OVE_LNX_PROG_ARENA_SIZE 0x18000u  /* 96K program heap */
#define OVE_LNX_NREG 2			  /* two regions: a parent + child image */
#define OVE_LNX_NSLOT 2

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
	/* Spawn slot `sidx` resuming at g_ove_lnx_vfork with r0 = r0val. */
	void (*spawn_resume)(int sidx, int ridx, long r0val);
	/* Abort (delete) slot `sidx`'s task. */
	void (*abort_slot)(int sidx);
	/* Sleep the run-loop task for `ms` milliseconds. */
	void (*sleep_ms)(unsigned ms);
};

/* ---- shared state (defined in ove_lnx_run.c) ------------------------------- */
extern struct ove_lnx_resume_ctx g_ove_lnx_vfork; /* vfork capture buffer */
extern ove_lnx_proc_t g_ove_lnx_proc[OVE_LNX_NSLOT];
extern int g_ove_lnx_used[OVE_LNX_NSLOT]; /* slot in use (run loop + seam read) */
extern volatile int g_ove_lnx_active;	  /* a run is in progress (seam trap gate) */

/* Where a parked program spins (in shared .text) until the run loop reaps it. */
void ove_lnx_park_loop(void);

/* The shared svc-dispatch body. Called by the seam's trap with the uniform frame
 * and the running slot's proc; on return the seam writes the frame back. */
void ove_lnx_dispatch(struct ove_lnx_frame *f, ove_lnx_proc_t *proc);

/* The shared run loop. Each engine's public ove_lnx_run() wraps this. */
int ove_lnx_run_common(const struct ove_lnx_engine *eng, const ove_lnx_run_config_t *cfg,
		       const char *path, int argc, const char *const argv[]);

#endif /* OVE_BACKENDS_COMMON_OVE_LNX_RUN_H */
