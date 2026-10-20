/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Test for the ready note of the NuttX scheduler-note driver: a woken
 * thread goes BLOCKED -> READY -> RUNNING, the switch notes arm_doirq()
 * raises after every interrupt change nothing, a kernel thread moves to
 * READY only from a wait, and the stop note that task_delete() raises
 * outside the kernel's critical section is handled inside one.
 *
 * The observation code is extracted verbatim from
 * backends/nuttx/nuttx_thread.c (tests/cmake/extract_nuttx_obs_block.py)
 * and compiled against the NuttX stand-in in nuttx_note_shim.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ove_config.h"

#include "nuttx_note_shim.h"

#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

#include "ove_nuttx_obs_block.inc"

static int failures;
static uint64_t fake_now;
/* Clock reads made outside a critical section: every one of them is a
 * tracker or kernel-thread table update the scheduler could interleave. */
static int unlocked_clock_reads;

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
	if (ove_test_nuttx_csection_depth == 0)
		unlocked_clock_reads++;
	return ++fake_now;
}

uintptr_t ove_backend_thread_current_handle(void)
{
	return (uintptr_t)ove_test_nuttx_current_tcb;
}

/* The kernel raises the switch and ready notes inside its critical
 * section. */
#define KERNEL_NOTE(op, tcb)                     \
	do {                                     \
		ove_test_nuttx_csection_depth++; \
		obs_note_ops.op(NULL, (tcb));    \
		ove_test_nuttx_csection_depth--; \
	} while (0)

#define MAX_RECORDS 64

static struct ove_trace_record records[MAX_RECORDS];
static size_t n_records;

static void drain(void)
{
	n_records = ove_trace_ring_drain(records, MAX_RECORDS);
}

/* The number of @from -> @to state records for @tid since the last
 * drain. */
static size_t count(uint32_t tid, uint32_t from, uint32_t to)
{
	size_t c = 0;
	for (size_t i = 0; i < n_records; i++)
		if (records[i].kind == OVE_TRACE_KIND_STATE && records[i].tid == tid &&
		    records[i].arg == from && records[i].code == to)
			c++;
	return c;
}

static void test_tracked_wake(void)
{
	static struct ove_thread t;
	static struct tcb_s tcb;

	memset(&t, 0, sizeof(t));
	memset(&tcb, 0, sizeof(tcb));
	tcb.pid = 50;
	tcb.task_state = TSTATE_TASK_RUNNING;
	ove_test_nuttx_current_tcb = &tcb;
	ove_test_nuttx_current_pid = tcb.pid;
	const uint32_t tid = (uint32_t)(uintptr_t)&t;

	ove_state_track_init(&t.st, OVE_THREAD_STATE_READY);
	obs_register(&t);
	ove_test_nuttx_csection_depth++;
	obs_transition(&t, OVE_THREAD_STATE_RUNNING);
	ove_test_nuttx_csection_depth--;
	drain();

	/* The thread waits, is woken, then takes the CPU. */
	tcb.task_state = TSTATE_WAIT_SEM;
	KERNEL_NOTE(suspend, &tcb);
	tcb.task_state = TSTATE_TASK_READYTORUN;
	KERNEL_NOTE(ready, &tcb);
	tcb.task_state = TSTATE_TASK_RUNNING;
	KERNEL_NOTE(resume, &tcb);
	drain();
	CHECK(count(tid, OVE_THREAD_STATE_RUNNING, OVE_THREAD_STATE_BLOCKED) == 1);
	CHECK(count(tid, OVE_THREAD_STATE_BLOCKED, OVE_THREAD_STATE_READY) == 1);
	CHECK(count(tid, OVE_THREAD_STATE_READY, OVE_THREAD_STATE_RUNNING) == 1);
	CHECK(count(tid, OVE_THREAD_STATE_BLOCKED, OVE_THREAD_STATE_RUNNING) == 0);

	/* arm_doirq() switches from a task to itself after every
	 * interrupt. */
	KERNEL_NOTE(suspend, &tcb);
	KERNEL_NOTE(resume, &tcb);
	drain();
	CHECK(n_records == 0);

	ove_test_nuttx_csection_depth++;
	obs_transition(&t, OVE_THREAD_STATE_TERMINATED);
	ove_test_nuttx_csection_depth--;
	obs_unregister(&t);
	tcb.task_state = TSTATE_TASK_INACTIVE;
	KERNEL_NOTE(stop, &tcb);
	drain();
}

static void test_kthread_ready(void)
{
	static struct tcb_s k;

	memset(&k, 0, sizeof(k));
	k.pid = 60;
	k.task_state = TSTATE_TASK_READYTORUN;
	const uint32_t tid = (uint32_t)(uintptr_t)&k;

	/* A thread first seen in a ready note (task_activate()) is not a
	 * wake: no record and no kernel-thread slot. */
	KERNEL_NOTE(ready, &k);
	drain();
	CHECK(n_records == 0);

	k.task_state = TSTATE_TASK_RUNNING;
	KERNEL_NOTE(resume, &k);
	k.task_state = TSTATE_WAIT_SEM;
	KERNEL_NOTE(suspend, &k);
	k.task_state = TSTATE_TASK_READYTORUN;
	KERNEL_NOTE(ready, &k);
	drain();
	CHECK(count(tid, OVE_THREAD_STATE_RUNNING, OVE_THREAD_STATE_BLOCKED) == 1);
	CHECK(count(tid, OVE_THREAD_STATE_BLOCKED, OVE_THREAD_STATE_READY) == 1);

	/* Reinserted while READY (a priority change): no record. */
	KERNEL_NOTE(ready, &k);
	drain();
	CHECK(n_records == 0);

	k.task_state = TSTATE_TASK_INACTIVE;
	KERNEL_NOTE(stop, &k);
	drain();
	CHECK(count(tid, OVE_THREAD_STATE_READY, OVE_THREAD_STATE_TERMINATED) == 1);
}

static void test_stop_outside_critical_section(void)
{
	static struct tcb_s k;

	memset(&k, 0, sizeof(k));
	k.pid = 70;
	k.task_state = TSTATE_TASK_RUNNING;
	KERNEL_NOTE(resume, &k);
	k.task_state = TSTATE_WAIT_SEM;
	KERNEL_NOTE(suspend, &k);
	drain();

	/* nxtask_terminate() raises the stop note after
	 * leave_critical_section(). */
	unlocked_clock_reads = 0;
	k.task_state = TSTATE_TASK_INACTIVE;
	obs_note_ops.stop(NULL, &k);
	drain();
	CHECK(count((uint32_t)(uintptr_t)&k, OVE_THREAD_STATE_BLOCKED,
		    OVE_THREAD_STATE_TERMINATED) == 1);
	CHECK(unlocked_clock_reads == 0);
	CHECK(ove_test_nuttx_csection_depth == 0);
}

int main(void)
{
	test_tracked_wake();
	test_kthread_ready();
	test_stop_outside_critical_section();

	if (failures != 0) {
		fprintf(stderr, "nuttx note ready: %d check(s) failed\n", failures);
		return 1;
	}
	printf("nuttx note ready: all checks passed\n");
	return 0;
}
