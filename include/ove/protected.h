/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_PROTECTED_H
#define OVE_PROTECTED_H

/**
 * @file protected.h
 * @defgroup ove_protected Protected Tasks
 * @ingroup ove_mem
 * @brief Fault-contained execution of untrusted code.
 *
 * Runs an entry function such that a memory-protection fault inside it is
 * trapped and reported to the supervisor rather than taking down the system —
 * the substrate for sandboxing third-party / loaded code and, later, for the
 * Linux personality's unprivileged user tasks.
 *
 * Enforcement is per-backend:
 * - NuttX (ARMv7-M): only an access to the backend's own 32-byte no-access
 *   MPU guard region, armed for the run, is trapped (as a MemManage fault,
 *   recovered by resuming at a thunk that longjmp()s back).  The entry stays
 *   privileged with the default memory map, so any other stray load or
 *   store goes through unchecked, and bus, usage and hard faults are not
 *   contained: this exercises the mechanism, it is not a sandbox.  The
 *   recovery state is global, so runs must not overlap.
 * - On the host test backend the process MMU enforces protection and a
 *   @c SIGSEGV / @c SIGBUS handler converts a violation into a contained fault;
 *   runs on different threads must not overlap either.
 * - FreeRTOS and Zephyr have no implementation.
 *
 * Phase-0 scope: **fault containment** (a violation is caught, the supervisor
 * survives). Per-region MPU grants / the unprivileged-task model are layered
 * on with the target engine seam and will extend this API.
 *
 * @note Requires @c CONFIG_OVE_PROTECTED.
 * @{
 */

#include <stddef.h>

#include "ove/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Entry function for a protected task.
 *
 * Runs synchronously on the thread that calls ove_ptask_run().  A contained
 * fault abandons it by longjmp, so anything it holds at that point (locks,
 * allocations) stays held.
 */
typedef void (*ove_ptask_fn)(void *arg);

/** Outcome of running a protected task. */
typedef enum ove_ptask_result {
	OVE_PTASK_OK = 0,    /**< Entry returned normally. */
	OVE_PTASK_FAULT = 1, /**< Entry trapped on a memory-protection violation. */
} ove_ptask_result_t;

/**
 * @brief Run @p entry in a fault-contained context.
 *
 * If @p entry triggers a memory-protection fault, the fault is trapped, the
 * supervisor regains control, and @p result is set to @c OVE_PTASK_FAULT.
 * Otherwise @p result is @c OVE_PTASK_OK.
 *
 * @param[in]  entry  Function to run.
 * @param[in]  arg    Opaque argument passed to @p entry.
 * @param[out] result Outcome; may be NULL.
 * @return OVE_OK if the run was performed (whether it completed or faulted),
 *         OVE_ERR_INVALID_PARAM if @p entry is NULL.
 * @note Requires @c CONFIG_OVE_PROTECTED.
 */
OVE_NODISCARD int ove_ptask_run(ove_ptask_fn entry, void *arg, ove_ptask_result_t *result);

/** @brief Number of faults contained since process start (diagnostics). */
unsigned long ove_ptask_fault_count(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_PROTECTED_H */
