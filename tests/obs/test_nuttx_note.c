/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <stdio.h>

#include "nuttx_note_shim.h"
#include "nuttx_thread_obs.h"
#include "ove_trace_ring.h"

static int failures;
static unsigned records;
static irqstate_t record_depth;
static struct ove_trace_record previous, last;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

uint64_t ove_state_stats_now_us(void)
{
	static uint64_t now;
	return ++now;
}

bool ove_trace_ring_push(const struct ove_trace_record *record)
{
	previous = last;
	last = *record;
	record_depth = critical_depth;
	records++;
	return true;
}

static void notify(note_hook_t hook, struct tcb_s *tcb)
{
	if (hook)
		hook(&obs_note_driver, tcb);
}

static void check_transition(const struct ove_trace_record *record, const void *tid, int from,
			     int to)
{
	CHECK(record->tid == (uint32_t)(uintptr_t)tid && record->kind == OVE_TRACE_KIND_STATE &&
	      record->arg == from && record->code == to);
}

static void check_edge(unsigned count, const void *tid, int from, int to)
{
	CHECK(records == count);
	check_transition(&last, tid, from, to);
}

static void test_tracked_wake_and_exit(void)
{
	struct ove_thread thread;
	struct tcb_s tcb = {.pid = getpid(), .task_state = TSTATE_WAIT_SEM};
	ove_state_track_init(&thread.st, OVE_THREAD_STATE_RUNNING);
	obs_register(&thread);

	records = 0;
	notify(obs_note_driver.ops->suspend, &tcb);
	check_edge(1, &thread, OVE_THREAD_STATE_RUNNING, OVE_THREAD_STATE_BLOCKED);
	tcb.task_state = TSTATE_TASK_PENDING;
	notify(obs_note_driver.ops->ready, &tcb);
	check_edge(2, &thread, OVE_THREAD_STATE_BLOCKED, OVE_THREAD_STATE_READY);
	tcb.task_state = TSTATE_TASK_RUNNING;
	notify(obs_note_driver.ops->resume, &tcb);
	check_edge(3, &thread, OVE_THREAD_STATE_READY, OVE_THREAD_STATE_RUNNING);
	/* arm_doirq() can raise a suspend note without switching tasks. */
	notify(obs_note_driver.ops->suspend, &tcb);
	CHECK(records == 3);

	obs_close(&thread);
	check_edge(4, &thread, OVE_THREAD_STATE_RUNNING, OVE_THREAD_STATE_TERMINATED);

	records = 0;
	tcb.task_state = TSTATE_TASK_READYTORUN;
	notify(obs_note_driver.ops->suspend, &tcb);
	CHECK(records == 0);
	tcb.task_state = TSTATE_TASK_RUNNING;
	notify(obs_note_driver.ops->resume, &tcb);
	CHECK(records == 0);
	tcb.task_state = TSTATE_WAIT_SEM;
	notify(obs_note_driver.ops->suspend, &tcb);
	CHECK(records == 0);
	tcb.task_state = TSTATE_TASK_INVALID;
	notify(obs_note_driver.ops->suspend, &tcb);
	tcb.task_state = TSTATE_TASK_INACTIVE;
	notify(obs_note_driver.ops->stop, &tcb);
	CHECK(records == 0);
	CHECK(obs_find(tcb.pid) == NULL);
	/* A stop for this TCB's trace ID would emit if it still held a slot. */
	ove_trace_emit_kthread_state((uintptr_t)&tcb, OVE_THREAD_STATE_TERMINATED,
				     OVE_TRACE_KT_ALWAYS);
	CHECK(records == 0);
}

static void test_tracked_kill(void)
{
	struct ove_thread thread;
	struct tcb_s tcb = {.pid = getpid(), .task_state = TSTATE_WAIT_SEM};
	ove_state_track_init(&thread.st, OVE_THREAD_STATE_BLOCKED);
	obs_register(&thread);

	records = 0;
	notify(obs_note_driver.ops->stop, &tcb);
	check_edge(1, &thread, OVE_THREAD_STATE_BLOCKED, OVE_THREAD_STATE_TERMINATED);
	CHECK(record_depth == 1 && critical_depth == 0);
	CHECK(obs_find(tcb.pid) == NULL);
}

static void test_stop_releases_preregistration_slot(void)
{
	struct ove_thread thread;
	struct tcb_s tcb = {.pid = getpid(), .task_state = TSTATE_TASK_READYTORUN};

	records = 0;
	notify(obs_note_driver.ops->suspend, &tcb);
	check_edge(1, &tcb, OVE_THREAD_STATE_RUNNING, OVE_THREAD_STATE_READY);
	tcb.task_state = TSTATE_TASK_RUNNING;
	notify(obs_note_driver.ops->resume, &tcb);
	check_edge(2, &tcb, OVE_THREAD_STATE_READY, OVE_THREAD_STATE_RUNNING);

	ove_state_track_init(&thread.st, OVE_THREAD_STATE_RUNNING);
	obs_register(&thread);
	tcb.task_state = TSTATE_TASK_INACTIVE;
	notify(obs_note_driver.ops->stop, &tcb);
	check_edge(4, &tcb, OVE_THREAD_STATE_RUNNING, OVE_THREAD_STATE_TERMINATED);
	check_transition(&previous, &thread, OVE_THREAD_STATE_RUNNING, OVE_THREAD_STATE_TERMINATED);
	CHECK(obs_find(tcb.pid) == NULL);

	records = 0;
	ove_trace_emit_kthread_state((uintptr_t)&tcb, OVE_THREAD_STATE_TERMINATED,
				     OVE_TRACE_KT_ALWAYS);
	CHECK(records == 0);
}

static void test_kernel_wake_and_stop(void)
{
	struct tcb_s tcb = {.pid = getpid() + 1, .task_state = TSTATE_TASK_READYTORUN};
	records = 0;
	notify(obs_note_driver.ops->ready, &tcb);
	CHECK(records == 0);
	tcb.task_state = TSTATE_WAIT_SEM;
	notify(obs_note_driver.ops->suspend, &tcb);
	check_edge(1, &tcb, OVE_THREAD_STATE_RUNNING, OVE_THREAD_STATE_BLOCKED);
	tcb.task_state = TSTATE_TASK_READYTORUN;
	notify(obs_note_driver.ops->ready, &tcb);
	check_edge(2, &tcb, OVE_THREAD_STATE_BLOCKED, OVE_THREAD_STATE_READY);
	tcb.task_state = TSTATE_TASK_INVALID;
	notify(obs_note_driver.ops->suspend, &tcb);
	CHECK(records == 2);
	tcb.task_state = TSTATE_TASK_INACTIVE;
	notify(obs_note_driver.ops->stop, &tcb);
	check_edge(3, &tcb, OVE_THREAD_STATE_READY, OVE_THREAD_STATE_TERMINATED);
}

int main(void)
{
	test_tracked_wake_and_exit();
	test_tracked_kill();
	test_stop_releases_preregistration_slot();
	test_kernel_wake_and_stop();
	if (failures)
		return 1;
	printf("test_nuttx_note: OK\n");
	return 0;
}
