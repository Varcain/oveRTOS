/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Unit tests for the MQTT wildcard topic matching logic.
 *
 * The mqtt_topic_matches() function is static in ove_net_mqtt.c,
 * so we replicate it here for isolated unit testing.
 */

#include "../framework/ove_test.h"
#include <string.h>

/* ── Replicate the matching function for testability ───────────── */

static int mqtt_topic_matches(const char *filter, size_t flen,
			      const char *topic, size_t tlen)
{
	size_t fi = 0, ti = 0;

	while (fi < flen && ti < tlen) {
		if (filter[fi] == '#')
			return 1;

		if (filter[fi] == '+') {
			while (ti < tlen && topic[ti] != '/')
				ti++;
			fi++;
			continue;
		}

		if (filter[fi] != topic[ti])
			return 0;

		fi++;
		ti++;
	}

	if (fi == flen && ti == tlen)
		return 1;

	if (fi + 1 < flen && filter[fi] == '/' && filter[fi + 1] == '#')
		return (ti == tlen);

	if (fi < flen && filter[fi] == '#')
		return 1;

	return 0;
}

/* ── Helper macro ──────────────────────────────────────────────── */

#define MATCH(filter, topic) \
	mqtt_topic_matches(filter, strlen(filter), topic, strlen(topic))

/* ── Exact match tests ─────────────────────────────────────────── */

static void test_exact_match(void **state)
{
	(void)state;
	assert_true(MATCH("sensor/temp", "sensor/temp"));
	assert_true(MATCH("a/b/c", "a/b/c"));
	assert_true(MATCH("x", "x"));
}

static void test_exact_mismatch(void **state)
{
	(void)state;
	assert_false(MATCH("sensor/temp", "sensor/humidity"));
	assert_false(MATCH("a/b", "a/b/c"));
	assert_false(MATCH("a/b/c", "a/b"));
	assert_false(MATCH("x", "y"));
}

/* ── Single-level wildcard (+) ─────────────────────────────────── */

static void test_plus_single_level(void **state)
{
	(void)state;
	assert_true(MATCH("sensor/+/data", "sensor/temp/data"));
	assert_true(MATCH("sensor/+/data", "sensor/humidity/data"));
	assert_true(MATCH("+/b/c", "a/b/c"));
	assert_true(MATCH("a/+/c", "a/x/c"));
	assert_true(MATCH("a/b/+", "a/b/c"));
}

static void test_plus_no_match(void **state)
{
	(void)state;
	/* '+' must not match multi-level */
	assert_false(MATCH("sensor/+", "sensor/temp/data"));
	assert_false(MATCH("+", "a/b"));
}

static void test_plus_multiple(void **state)
{
	(void)state;
	assert_true(MATCH("+/+/+", "a/b/c"));
	assert_true(MATCH("+/+", "x/y"));
	assert_false(MATCH("+/+", "a/b/c"));
}

/* ── Multi-level wildcard (#) ──────────────────────────────────── */

static void test_hash_all(void **state)
{
	(void)state;
	assert_true(MATCH("#", "anything"));
	assert_true(MATCH("#", "a/b/c/d"));
	assert_true(MATCH("#", ""));
}

static void test_hash_suffix(void **state)
{
	(void)state;
	assert_true(MATCH("sensor/#", "sensor/temp"));
	assert_true(MATCH("sensor/#", "sensor/temp/data"));
	assert_true(MATCH("sensor/#", "sensor"));
	assert_true(MATCH("a/b/#", "a/b/c/d/e"));
}

static void test_hash_no_match(void **state)
{
	(void)state;
	assert_false(MATCH("sensor/#", "other/temp"));
	assert_false(MATCH("a/b/#", "a/c"));
}

/* ── Combined wildcards ────────────────────────────────────────── */

static void test_combined(void **state)
{
	(void)state;
	assert_true(MATCH("+/temp/#", "sensor/temp/data"));
	assert_true(MATCH("+/temp/#", "device/temp"));
	assert_true(MATCH("+/+/#", "a/b/c/d"));
}

/* ── Edge cases ────────────────────────────────────────────────── */

static void test_empty_filter(void **state)
{
	(void)state;
	assert_true(MATCH("", ""));
	assert_false(MATCH("", "notempty"));
}

static void test_trailing_slash(void **state)
{
	(void)state;
	assert_false(MATCH("a/b", "a/b/"));
	/* '+' does not match empty level (trailing slash) per most brokers */
	assert_false(MATCH("a/b/+", "a/b/"));
	/* '#' does match trailing slash */
	assert_true(MATCH("a/b/#", "a/b/"));
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_net_mqtt_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_exact_match),
		cmocka_unit_test(test_exact_mismatch),
		cmocka_unit_test(test_plus_single_level),
		cmocka_unit_test(test_plus_no_match),
		cmocka_unit_test(test_plus_multiple),
		cmocka_unit_test(test_hash_all),
		cmocka_unit_test(test_hash_suffix),
		cmocka_unit_test(test_hash_no_match),
		cmocka_unit_test(test_combined),
		cmocka_unit_test(test_empty_filter),
		cmocka_unit_test(test_trailing_slash),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
