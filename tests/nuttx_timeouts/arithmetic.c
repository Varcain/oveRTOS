/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include OVE_TEST_CONVERSION
#include <nuttx/wdog.h>

static unsigned checks, failures;
static void check(int valid, const char *name)
{
	++checks;
	if (!valid) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
static uint64_t expected(uint64_t ns)
{
	uint64_t ticks = ns / NSEC_PER_TICK + (ns % NSEC_PER_TICK != 0);
	return ticks > UINT32_MAX ? UINT32_MAX : ticks;
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "canary"))
		check(0, "early failure survives later controls");
	const uint64_t threshold = UINT32_MAX - (NSEC_PER_TICK - 1);
	const uint64_t values[] = {0,
				   1,
				   NSEC_PER_TICK - 1,
				   NSEC_PER_TICK,
				   NSEC_PER_TICK + 1,
				   threshold - 1,
				   threshold,
				   threshold + 1,
				   ((uint64_t)UINT32_MAX - 1) * NSEC_PER_TICK,
				   (uint64_t)UINT32_MAX * NSEC_PER_TICK,
				   (uint64_t)UINT32_MAX * NSEC_PER_TICK + 1,
				   UINT64_MAX - (NSEC_PER_TICK - 1),
				   UINT64_MAX - (NSEC_PER_TICK - 1) + 1,
				   UINT64_MAX - 1,
				   UINT64_MAX};
	for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
		uint64_t actual = ove_ns_to_ticks(values[i]), want = expected(values[i]);
		if (actual != want)
			printf("ceil input=%" PRIu64 " expected=%" PRIu64 " actual=%" PRIu64 "\n",
			       values[i], want, actual);
		check(actual == want, "general ceil preserves large finite values");
	}
#if OVE_TEST_SAFE_WAIT
	const uint64_t cap = (uint64_t)WDOG_MAX_DELAY - 2 < (uint64_t)INT32_MAX - 1
				     ? (uint64_t)WDOG_MAX_DELAY - 2
				     : (uint64_t)INT32_MAX - 1;
	for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
		if (values[i] == UINT64_MAX)
			continue; /* FOREVER is admitted by callers, not this finite helper. */
		uint64_t want = expected(values[i]);
		if (want > cap)
			want = cap;
		check(ove_ns_to_wait_ticks(values[i]) == want, "wait cap preserves retry headroom");
	}
	uint64_t delay = ove_ns_to_wait_ticks(UINT64_MAX - 1);
	check(delay > 0 && delay + 1 < (uint64_t)WDOG_MAX_DELAY,
	      "same-tick retry remains watchdog-admissible");
	check(delay + 1 <= INT32_MAX, "same-tick retry remains nonnegative after native narrowing");
	clock_t now = (clock_t)(CLOCK_MAX - 2), end = (clock_t)(now + (clock_t)delay);
	check(!clock_compare(end, now), "finite deadline stays future across clock wrap");
	check(clock_compare(end, end), "deadline equality expires");
	const uint64_t edges[] = {cap,
				  cap + 1,
				  (uint64_t)WDOG_MAX_DELAY - 2,
				  (uint64_t)WDOG_MAX_DELAY - 1,
				  (uint64_t)WDOG_MAX_DELAY,
				  (uint64_t)INT32_MAX - 1,
				  INT32_MAX,
				  (uint64_t)INT32_MAX + 1};
	for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
		uint64_t candidate = edges[i];
		/* A native uint32 argument, watchdog admission and same-tick
		 * signed retry must all admit an initial delay. Observe rejected
		 * ranges as integers only; no native watchdog is linked. */
		bool admissible = candidate <= UINT32_MAX && candidate < WDOG_MAX_DELAY &&
				  candidate + 1 < WDOG_MAX_DELAY && candidate + 1 <= INT32_MAX;
		check(admissible == (candidate <= cap), "native initial-admission boundary model");
		uint64_t ns = candidate > (UINT64_MAX - 1) / NSEC_PER_TICK
				      ? UINT64_MAX - 1
				      : candidate * NSEC_PER_TICK;
		uint32_t selected = ove_ns_to_wait_ticks(ns);
		uint64_t want = expected(ns);
		check(selected == (want > cap ? cap : want), "conversion at native retry boundary");
		for (unsigned elapsed = 0; elapsed <= 2; elapsed += 2) {
			clock_t start = (clock_t)(CLOCK_MAX - 1);
			clock_t native_end = (clock_t)(start + selected + 1);
			uint64_t retry = (uint32_t)(native_end - (clock_t)(start + elapsed));
			check(retry < WDOG_MAX_DELAY && (int32_t)retry >= 0,
			      "same/later-tick EINTR retry remains admissible across wrap");
			check(retry == selected + 1 - elapsed,
			      "native retry keeps one captured phase-safe deadline");
		}
	}

#endif
	printf("nuttx timeout arithmetic: clock=%u tick=%u checks=%u failures=%u\n", OVE_CLOCK_BITS,
	       OVE_TICK_NS, checks, failures);
	return failures != 0;
}
