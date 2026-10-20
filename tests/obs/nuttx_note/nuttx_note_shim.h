/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Minimal NuttX stand-in for the scheduler-note observation code of
 * backends/nuttx/nuttx_thread.c.
 *
 * The block under test (obs_tasks[], obs_find(), obs_transition(),
 * obs_note_*, obs_register(), obs_unregister()) is extracted VERBATIM out
 * of the repository's backends/nuttx/nuttx_thread.c by
 * tests/cmake/extract_nuttx_obs_block.py and #included by
 * test_nuttx_note_exit.c.  This header only provides the NuttX surface
 * that block touches, so the real code can be compiled and driven on the
 * host.
 *
 * The note_driver_ops_s layout mirrors
 *   dl/nuttx-24f210ef/include/nuttx/note/note_driver.h
 * plus the .ready op added by
 *   config/patches/nuttx/0002-sched-note-let-drivers-observe-wakeups.patch
 * NuttX dispatches the ops through
 *   ((drv)->ops->stop && ((drv)->ops->stop(drv, tcb), true))
 * so a NULL op is legal (that is what "drop the .stop op" means).
 */

#ifndef OVE_TEST_NUTTX_NOTE_SHIM_H
#define OVE_TEST_NUTTX_NOTE_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FAR
#define OK 0
#define ERROR (-1)

typedef unsigned int irqstate_t;

/* NuttX task states (include/nuttx/sched.h), non-SMP numbering. */
#define TSTATE_TASK_INVALID 0
#define TSTATE_TASK_PENDING 1
#define TSTATE_TASK_READYTORUN 2
#define TSTATE_TASK_RUNNING 3
#define TSTATE_TASK_INACTIVE 4
#define TSTATE_WAIT_SEM 5

/* The subset of struct tcb_s the observation code reads. */
struct tcb_s {
	int pid;
	uint8_t task_state;
	const char *name;
};

struct note_driver_s;

struct note_driver_ops_s {
	void (*add)(FAR struct note_driver_s *drv, FAR const void *note, size_t notelen);
	void (*start)(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb);
	void (*stop)(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb);
	void (*suspend)(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb);
	void (*resume)(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb);
	void (*ready)(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb);
};

struct note_driver_s {
	FAR const struct note_driver_ops_s *ops;
};

/* ── Fake NuttX task context ────────────────────────────────────────── */

/* The TCB of the task the test pretends to run in. */
extern FAR struct tcb_s *ove_test_nuttx_current_tcb;
/* The pid of that task (NuttX getpid()). */
extern pid_t ove_test_nuttx_current_pid;

/* The depth of the critical section the test pretends to be in.  The
 * observation code may touch the trackers and the kernel-thread table only
 * inside one. */
extern int ove_test_nuttx_csection_depth;

static inline irqstate_t enter_critical_section(void)
{
	ove_test_nuttx_csection_depth++;
	return 0;
}

static inline void leave_critical_section(irqstate_t flags)
{
	(void)flags;
	ove_test_nuttx_csection_depth--;
}

static inline pid_t getpid(void)
{
	return ove_test_nuttx_current_pid;
}

/* NuttX's canonical "TCB of the running task" accessor
 * (dl/nuttx-24f210ef/sched/sched/sched_self.c). */
static inline FAR struct tcb_s *nxsched_self(void)
{
	return ove_test_nuttx_current_tcb;
}

static inline int note_driver_register(FAR struct note_driver_s *driver)
{
	(void)driver;
	return OK;
}

#endif /* OVE_TEST_NUTTX_NOTE_SHIM_H */
