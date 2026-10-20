/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Regression test for the NuttX scheduler-note driver: the exit of a
 * tracked ove thread must not create a lane of its own, and the exit of a
 * kernel thread must free its kernel-thread slot.
 *
 * NuttX raises the notes of a task that exits AFTER task_wrapper() has
 * returned:
 *
 *   sched/task/task_exit.c:119  nxsched_switch_context(dtcb, rtcb)
 *                               -> sched_note_suspend(dtcb)
 *   sched/task/task_exit.c:146  sched_note_stop(dtcb)
 *   sched/task/task_exit.c:147  nxsched_release_tcb(dtcb)
 *
 * A thread that ended normally is therefore no longer in the note driver's
 * task table when its notes arrive.  Reporting those notes through the
 * kernel-thread path invented a lane for a tid the guest never described
 * (a phantom empty lane in the dashboard).  A task that later inherits
 * the TCB address must still get a lane of its own.
 *
 * The scheduler-note observation code is not re-implemented here: the
 * block is extracted verbatim from backends/nuttx/nuttx_thread.c at build
 * time (tests/cmake/extract_nuttx_obs_block.py) and compiled against the
 * NuttX stand-in in nuttx_note_shim.h.  The kernel-thread table is the
 * real one (backends/common/ove_trace.c).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ove_config.h"

#include "nuttx_note_shim.h"

#include "ove/thread.h" /* pulls in ove/storage.h -> the NuttX storage header */
#include "ove/thread_state_stats.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

/* The scheduler-note observation code - extracted verbatim from
 * backends/nuttx/nuttx_thread.c (see tests/cmake/extract_nuttx_obs_block.py). */
#include "ove_nuttx_obs_block.inc"

static int failures;
static uint64_t fake_now;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

FAR struct tcb_s *ove_test_nuttx_current_tcb;
pid_t ove_test_nuttx_current_pid;
int ove_test_nuttx_csection_depth;

uint64_t ove_state_stats_now_us(void)
{
	return ++fake_now;
}

uintptr_t ove_backend_thread_current_handle(void)
{
	return (uintptr_t)ove_test_nuttx_current_tcb;
}

/* ── the trace stream the guest would publish ───────────────────────── */

#define MAX_RECORDS 64

static struct ove_trace_record records[MAX_RECORDS];
static size_t n_records;

/* Take the records produced since the last drain. */
static void drain(void)
{
	n_records = ove_trace_ring_drain(records, MAX_RECORDS);
}

/* True when a state record for @tid was produced since the last drain. */
static bool saw_tid(uint32_t tid)
{
	for (size_t i = 0; i < n_records; i++)
		if (records[i].kind == OVE_TRACE_KIND_STATE && records[i].tid == tid)
			return true;
	return false;
}

/* True when a state record for @tid carried the state @code. */
static bool saw_state(uint32_t tid, uint8_t code)
{
	for (size_t i = 0; i < n_records; i++)
		if (records[i].kind == OVE_TRACE_KIND_STATE && records[i].tid == tid &&
		    records[i].code == code)
			return true;
	return false;
}

/* ── a tracked thread that ends the normal way ──────────────────────── */

static void test_tracked_thread_exit(void)
{
	static struct ove_thread tracked;
	static struct tcb_s tcb;

	memset(&tracked, 0, sizeof(tracked));
	memset(&tcb, 0, sizeof(tcb));
	tcb.pid = 42;
	tcb.name = "producer";
	tcb.task_state = TSTATE_TASK_RUNNING;

	ove_test_nuttx_current_tcb = &tcb;
	ove_test_nuttx_current_pid = tcb.pid;

	const uint32_t thread_tid = (uint32_t)(uintptr_t)&tracked;
	const uint32_t tcb_tid = (uint32_t)(uintptr_t)&tcb;

	/* task_wrapper() on entry: the thread registers itself and marks
	 * itself RUNNING. */
	ove_state_track_init(&tracked.st, OVE_THREAD_STATE_READY);
	obs_register(&tracked);
	obs_transition(&tracked, OVE_THREAD_STATE_RUNNING);
	drain();

	/* The entry function returns: task_wrapper() closes the lane of the
	 * thread and leaves the table BEFORE it returns to NuttX. */
	obs_transition(&tracked, OVE_THREAD_STATE_TERMINATED);
	drain();
	CHECK(saw_state(thread_tid, OVE_THREAD_STATE_TERMINATED));
	obs_unregister(&tracked);
	drain();

	/* NuttX raises the suspend note of the exiting task
	 * (sched/task/task_exit.c:119, nxsched_switch_context()).  The task
	 * is gone, so the note belongs to no lane: the guest never
	 * described the tid of a TCB. */
	tcb.task_state = TSTATE_TASK_INACTIVE;
	obs_note_ops.suspend(NULL, &tcb);
	drain();
	CHECK(!saw_tid(tcb_tid));
	CHECK(n_records == 0);

	/* NuttX raises the stop note (sched/task/task_exit.c:146,
	 * sched_note_stop()).  Reporting it through the kernel-thread path
	 * would invent a lane as well. */
	obs_note_ops.stop(NULL, &tcb);
	drain();
	CHECK(!saw_tid(tcb_tid));
	CHECK(n_records == 0);

	/* NuttX releases the TCB (sched/task/task_exit.c:147) and the next
	 * task inherits the address.  Its notes must still be delivered. */
	tcb.pid = 43;
	tcb.name = "hpwork";
	tcb.task_state = TSTATE_TASK_RUNNING;
	ove_test_nuttx_current_tcb = &tcb;
	ove_test_nuttx_current_pid = tcb.pid;

	obs_note_ops.resume(NULL, &tcb);
	drain();
	tcb.task_state = TSTATE_WAIT_SEM;
	obs_note_ops.suspend(NULL, &tcb);
	drain();
	CHECK(saw_tid(tcb_tid));
	CHECK(saw_state(tcb_tid, OVE_THREAD_STATE_BLOCKED));
}

/* ── a tracked thread that is killed before it can end ──────────────── */

static void test_tracked_thread_killed(void)
{
	static struct ove_thread tracked;
	static struct tcb_s tcb;

	memset(&tracked, 0, sizeof(tracked));
	memset(&tcb, 0, sizeof(tcb));
	tcb.pid = 44;
	tcb.name = "producer";
	tcb.task_state = TSTATE_TASK_RUNNING;

	ove_test_nuttx_current_tcb = &tcb;
	ove_test_nuttx_current_pid = tcb.pid;

	const uint32_t thread_tid = (uint32_t)(uintptr_t)&tracked;
	const uint32_t tcb_tid = (uint32_t)(uintptr_t)&tcb;

	ove_state_track_init(&tracked.st, OVE_THREAD_STATE_READY);
	obs_register(&tracked);
	obs_transition(&tracked, OVE_THREAD_STATE_RUNNING);
	drain();

	/* The thread is killed: task_wrapper() never returns, so the thread
	 * is still in the table when NuttX raises the stop note.  The lane
	 * of the thread has to be closed here ... */
	obs_note_ops.stop(NULL, &tcb);
	drain();
	CHECK(saw_state(thread_tid, OVE_THREAD_STATE_TERMINATED));
	/* ... and the TCB address must not get a kernel-thread record. */
	CHECK(!saw_tid(tcb_tid));
}

/* ── a kernel thread: its notes must not be suppressed ──────────────── */

static void test_kernel_thread_notes(void)
{
	static struct tcb_s ktcb;

	memset(&ktcb, 0, sizeof(ktcb));
	ktcb.pid = 77;
	ktcb.name = "hpwork";
	ktcb.task_state = TSTATE_TASK_RUNNING;

	const uint32_t ktcb_tid = (uint32_t)(uintptr_t)&ktcb;

	obs_note_ops.resume(NULL, &ktcb);
	drain();
	ktcb.task_state = TSTATE_WAIT_SEM;
	obs_note_ops.suspend(NULL, &ktcb);
	drain();
	CHECK(saw_state(ktcb_tid, OVE_THREAD_STATE_BLOCKED));

	obs_note_ops.ready(NULL, &ktcb);
	drain();
	CHECK(saw_state(ktcb_tid, OVE_THREAD_STATE_READY));

	obs_note_ops.stop(NULL, &ktcb);
	drain();
	CHECK(saw_state(ktcb_tid, OVE_THREAD_STATE_TERMINATED));
}

int main(void)
{
	drain();

	test_tracked_thread_exit();
	test_tracked_thread_killed();
	test_kernel_thread_notes();

	if (failures != 0) {
		fprintf(stderr, "nuttx note exit: %d check(s) failed\n", failures);
		return 1;
	}
	printf("nuttx note exit: all checks passed\n");
	return 0;
}
