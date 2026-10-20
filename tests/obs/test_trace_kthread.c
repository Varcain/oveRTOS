/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host test for the kernel-thread state table (backends/common/ove_trace.c,
 * ove_trace_emit_kthread_state()): the no-op first observations an ove
 * thread raises before its own tracker exists must not take a slot, an
 * exited thread must free its slot, and the "when" modes must hold.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ove/thread.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

static int records;
static struct ove_trace_record last;

bool ove_trace_ring_push(const struct ove_trace_record *r)
{
	last = *r;
	records++;
	return true;
}

uint64_t ove_state_stats_now_us(void)
{
	static uint64_t clock;
	return ++clock;
}

uintptr_t ove_backend_thread_current_handle(void)
{
	return 0;
}

/* Many ove threads pass through the kernel-thread path before their own
 * tracker exists: a wake-up of a thread that is not waiting, and a first
 * switch-in.  Neither is emitted, so neither may use up a slot. */
static void test_no_op_observations_take_no_slot(void)
{
	for (uintptr_t t = 0x1000; t < 0x1000 + 32; t++) {
		ove_trace_emit_kthread_state(t, OVE_THREAD_STATE_READY, OVE_TRACE_KT_FROM_WAITING);
		ove_trace_emit_kthread_state(t, OVE_THREAD_STATE_RUNNING, OVE_TRACE_KT_ALWAYS);
	}
	records = 0;
	ove_trace_emit_kthread_state(0x9000, OVE_THREAD_STATE_READY, OVE_TRACE_KT_ALWAYS);
	CHECK(records == 1); /* idle still gets its lane */
	CHECK(last.tid == 0x9000 && last.object == OVE_THREAD_STATE_RUNNING &&
	      last.code == OVE_THREAD_STATE_READY);
}

/* An exited thread frees its slot, and a new thread at the same address
 * starts from scratch. */
static void test_exit_frees_the_slot(void)
{
	ove_trace_emit_kthread_state(0xA000, OVE_THREAD_STATE_BLOCKED, OVE_TRACE_KT_ALWAYS);
	ove_trace_emit_kthread_state(0xA000, OVE_THREAD_STATE_TERMINATED, OVE_TRACE_KT_ALWAYS);
	records = 0;
	ove_trace_emit_kthread_state(0xA000, OVE_THREAD_STATE_BLOCKED, OVE_TRACE_KT_ALWAYS);
	CHECK(records == 1);
	CHECK(last.object == OVE_THREAD_STATE_RUNNING); /* a new thread, assumed running */

	/* Slots are reusable: more exits than the table holds still work. */
	for (uintptr_t t = 0xB000; t < 0xB000 + 64; t++) {
		ove_trace_emit_kthread_state(t, OVE_THREAD_STATE_BLOCKED, OVE_TRACE_KT_ALWAYS);
		ove_trace_emit_kthread_state(t, OVE_THREAD_STATE_TERMINATED, OVE_TRACE_KT_ALWAYS);
	}
	records = 0;
	ove_trace_emit_kthread_state(0xC000, OVE_THREAD_STATE_BLOCKED, OVE_TRACE_KT_ALWAYS);
	CHECK(records == 1);
}

/* FROM_RUNNING: a switch-out reports a preemption only for a running
 * thread, so a kernel thread that blocked stays BLOCKED. */
static void test_from_running(void)
{
	ove_trace_emit_kthread_state(0xD000, OVE_THREAD_STATE_BLOCKED, OVE_TRACE_KT_ALWAYS);
	records = 0;
	ove_trace_emit_kthread_state(0xD000, OVE_THREAD_STATE_READY, OVE_TRACE_KT_FROM_RUNNING);
	CHECK(records == 0);
	ove_trace_emit_kthread_state(0xD000, OVE_THREAD_STATE_RUNNING, OVE_TRACE_KT_ALWAYS);
	ove_trace_emit_kthread_state(0xD000, OVE_THREAD_STATE_READY, OVE_TRACE_KT_FROM_RUNNING);
	CHECK(records == 2 && last.code == OVE_THREAD_STATE_READY);
}

/* A marker carries the full object address; a state record carries the
 * old state at the v2 offset (the dashboard reads it there) and in the
 * object word (other decoders read it there); the pad stays zero. */
static void test_record_carries_full_object_and_old_state(void)
{
	ove_trace_emit_mark(0x7000, OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_POST, 0x2000beefu);
	CHECK(last.kind == OVE_TRACE_KIND_MARK && last.object == 0x2000beefu && last.reserved == 0);
	CHECK(last.code == ((OVE_TRACE_PRIM_QUEUE << 4) | OVE_TRACE_ACT_POST));
	CHECK(last.pad == 0);
	ove_trace_emit_state_at(0x7000, OVE_THREAD_STATE_BLOCKED, OVE_THREAD_STATE_READY, 42);
	CHECK(last.kind == OVE_TRACE_KIND_STATE && last.code == OVE_THREAD_STATE_READY);
	CHECK(last.reserved == OVE_THREAD_STATE_BLOCKED && last.object == OVE_THREAD_STATE_BLOCKED);
	CHECK(last.pad == 0);
}

int main(void)
{
	test_record_carries_full_object_and_old_state();
	test_no_op_observations_take_no_slot();
	test_exit_frees_the_slot();
	test_from_running();

	if (failures) {
		fprintf(stderr, "test_trace_kthread: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_trace_kthread: OK\n");
	return 0;
}
