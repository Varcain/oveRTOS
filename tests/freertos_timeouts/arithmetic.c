/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include OVE_TEST_CONVERSION

#define NS_PER_SECOND UINT64_C(1000000000)
static unsigned checks, failures;
static void check(bool valid, const char *name)
{
	++checks;
	if (!valid) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
/* Host-only independent oracle. Target implementation must not require uint128. */
static uint64_t oracle(uint64_t ns)
{
	if (ns == UINT64_MAX)
		return portMAX_DELAY;
	__uint128_t numerator = (__uint128_t)ns * configTICK_RATE_HZ;
	__uint128_t result = (numerator + NS_PER_SECOND - 1) / NS_PER_SECOND;
	uint64_t limit = (uint64_t)portMAX_DELAY - 1;
	return result > limit ? limit : (uint64_t)result;
}
static uint64_t finite_ns(__uint128_t value)
{
	return value > UINT64_MAX - 1 ? UINT64_MAX - 1 : (uint64_t)value;
}
static int compare(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
	return (a > b) - (a < b);
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "canary"))
		check(false, "early failure survives later controls");
	const uint64_t rate = configTICK_RATE_HZ, limit = (uint64_t)portMAX_DELAY - 1;
	const uint64_t quantum = NS_PER_SECOND / rate + (NS_PER_SECOND % rate != 0);
	const uint64_t cap_ns = finite_ns((__uint128_t)limit * NS_PER_SECOND / rate);
	const uint64_t whole = rate / NS_PER_SECOND, part = rate % NS_PER_SECOND;
	const uint64_t first = whole ? limit / (whole ? whole : 1) : UINT64_MAX - 1;
	const uint64_t second =
		part ? finite_ns((__uint128_t)(limit / (part ? part : 1)) * NS_PER_SECOND)
		     : UINT64_MAX - 1;
	uint64_t values[] = {0,
			     1,
			     quantum - 1,
			     quantum,
			     quantum + 1,
			     NS_PER_SECOND - 1,
			     NS_PER_SECOND,
			     NS_PER_SECOND + 1,
			     UINT32_MAX - 1,
			     UINT32_MAX,
			     (uint64_t)UINT32_MAX + 1,
			     UINT64_C(3276700000),
			     UINT64_C(3276750000),
			     UINT64_C(3276800000),
			     UINT64_MAX - quantum,
			     UINT64_MAX - quantum + 1,
			     UINT64_MAX - 2,
			     UINT64_MAX - 1,
			     UINT64_MAX,
			     cap_ns ? cap_ns - 1 : 0,
			     cap_ns,
			     finite_ns((__uint128_t)cap_ns + 1),
			     first ? first - 1 : 0,
			     first,
			     finite_ns((__uint128_t)first + 1),
			     second ? second - 1 : 0,
			     second,
			     finite_ns((__uint128_t)second + 1),
			     finite_ns((__uint128_t)second + NS_PER_SECOND / 2),
			     finite_ns((__uint128_t)second + NS_PER_SECOND - 1)};
	const unsigned count = sizeof(values) / sizeof(values[0]);
	for (unsigned i = 0; i < count; ++i) {
		uint64_t ns = values[i], actual = ove_ns_to_ticks(ns), want = oracle(ns);
		if (actual != want)
			printf("ceil ns=%" PRIu64 " expected=%" PRIu64 " actual=%" PRIu64 "\n", ns,
			       want, actual);
		check(actual == want, "conversion matches exact rational oracle");
		check(ns == UINT64_MAX ? actual == portMAX_DELAY
				       : (ns == 0 ? actual == 0 : actual > 0 && actual <= limit),
		      "zero and encoded forever stay distinct from every positive finite input");
	}
	qsort(values, count, sizeof(values[0]), compare);
	uint64_t previous = ove_ns_to_ticks(values[0]);
	for (unsigned i = 1; i < count; ++i) {
		uint64_t next = ove_ns_to_ticks(values[i]);
		check(next >= previous, "conversion is monotone across finite boundaries");
		previous = next;
	}
	printf("freertos timeout arithmetic: bits=%u rate=%" PRIu64
	       " cases=%u checks=%u failures=%u\n",
	       OVE_TICK_BITS, rate, count, checks, failures);
	return failures != 0;
}
