/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "provider.h"
#include <stdio.h>
#include <string.h>

static ove_event_t event;
static unsigned cases, workers;
#ifdef CONFIG_OVE_ZERO_HEAP
static ove_event_storage_t backing;
#endif
static unsigned bits(unsigned mask)
{
	unsigned count = 0;
	for (; mask; mask &= mask - 1)
		++count;
	return count;
}
static void begin(void)
{
	++cases;
	workers = 0;
	fixture_reset();
#ifdef CONFIG_OVE_ZERO_HEAP
	fixture_check(ove_event_init(&event, &backing) == OVE_OK, "shared Event initializes");
#else
	fixture_check(ove_event_create(&event) == OVE_OK, "shared Event initializes");
#endif
	fixture_mode(!OVE_MODEL_MPU);
}
static void spawn(unsigned id, uint64_t budget)
{
	fixture_start(id, event, budget, !OVE_MODEL_MPU || id == 2);
	workers = id + 1;
	fixture_waiting(1u << id);
}
static void finish(void)
{
	for (unsigned i = 0; i < workers; ++i)
		fixture_suspend(i, false);
	/* Advance only after pending hints have been processed and every live
	 * call has entered its relative native wait. A ready task can otherwise
	 * start that wait after the clock advance and correctly need more time. */
	fixture_waiting(((1u << workers) - 1) & ~fixture_finished_mask());
	fixture_advance(1000000);
	fixture_join();
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_event_deinit(event);
#else
	ove_event_destroy(event);
#endif
	event = NULL;
}
static void release(unsigned count, unsigned route)
{
	unsigned all = (1u << workers) - 1;
	unsigned initial = bits(fixture_finished_mask() & all);
	for (unsigned i = 0; i < count; ++i) {
		unsigned live = all & ~fixture_finished_mask();
		fixture_check(live != 0, "one signal has at most one successful consumer");
		if (!live)
			break;
		fixture_waiting(live);
		fixture_prepare_signal();
		fixture_signal(event, route == 1, route == 2);
		fixture_await_signal();
		unsigned done = fixture_finished_mask() & all;
		fixture_check(bits(done) == initial + i + 1,
			      "each separated occurrence completes exactly one wait");
	}
}
static void separated(unsigned count, unsigned route)
{
	begin();
	for (unsigned i = 0; i < count; ++i)
		spawn(i, OVE_MS(100));
	release(count, route);
	fixture_finished((1u << count) - 1);
	for (unsigned i = 0; i < count; ++i)
		fixture_check(fixture_result(i) == OVE_OK,
			      "shared wait consumes its target signal");
	finish();
}
static void retire(unsigned id, bool abort)
{
	begin();
	for (unsigned i = 0; i < 3; ++i)
		spawn(i, i == id ? OVE_MS(10) : OVE_MS(100));
	if (abort)
		fixture_abort(id);
	else
		fixture_advance(10);
	fixture_finished(1u << id);
	fixture_check(fixture_result(id) == OVE_ERR_TIMEOUT,
		      "chosen invocation retires its own timeout or abort");
	release(2, 0);
	fixture_finished(7);
	for (unsigned i = 0; i < 3; ++i)
		if (i != id)
			fixture_check(fixture_result(i) == OVE_OK,
				      "head middle or tail retirement preserves other waits");
	finish();
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "canary"))
		fixture_check(false, "early failure survives later threaded controls");
	separated(2, 0);
	separated(3, 0);
	separated(2, 1);
	separated(2, 2);
	begin();
	spawn(0, OVE_MS(100));
	spawn(1, OVE_MS(100));
	release(1, 0);
	fixture_check(bits(fixture_finished_mask() & 3) == 1,
		      "one occurrence is not broadcast success");
	finish();
	begin();
	spawn(0, OVE_MS(100));
	fixture_check(ove_event_wait(event, 0) == OVE_ERR_TIMEOUT,
		      "empty poll completes without stealing a live registration");
	release(1, 0);
	fixture_finished(1);
	fixture_check(fixture_result(0) == OVE_OK, "signal still reaches the older waiter");
	finish();
	for (unsigned id = 0; id < 3; ++id)
		retire(id, false);
#if OVE_MODEL_ABORT
	for (unsigned id = 0; id < 3; ++id)
		retire(id, true);
#endif
	begin();
	spawn(0, OVE_MS(100));
	spawn(1, OVE_MS(100));
	fixture_suspend(1, true);
	release(1, 0);
	fixture_finished(1);
	fixture_check(fixture_result(0) == OVE_OK,
		      "suspended head does not absorb the eligible waiter's signal");
	fixture_suspend(1, false);
	release(1, 0);
	fixture_finished(3);
	fixture_check(fixture_result(1) == OVE_OK, "resumed waiter remains usable");
	finish();
	begin();
	spawn(0, OVE_MS(100));
	fixture_hint(0);
	fixture_waiting(1);
	fixture_check(!(fixture_finished_mask() & 1), "unrelated hint does not grant success");
	release(1, 0);
	fixture_finished(1);
	finish();
	begin();
	fixture_signal(event, false, false);
	fixture_signal(event, true, false);
	fixture_check(ove_event_wait(event, 0) == OVE_OK, "preposted token is sticky");
	fixture_check(ove_event_wait(event, 0) == OVE_ERR_TIMEOUT,
		      "pending signals coalesce into one occurrence");
	finish();
	printf("freertos event threaded: bits=%u mpu_model=%u abort=%u heap=%u cases=%u failures=%u\n",
	       OVE_MODEL_TICK_BITS, OVE_MODEL_MPU, OVE_MODEL_ABORT,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       cases, fixture_failures());
	return fixture_failures() != 0;
}
