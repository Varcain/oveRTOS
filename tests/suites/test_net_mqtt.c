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
 * The matcher was extracted to backends/common/ove_mqtt_topic.c so
 * tests link against the same code path as production clients; no
 * more duplicated logic here.
 */

#include "../framework/ove_test.h"
#include "ove_mqtt_topic.h"
#include <string.h>

/* ── Helper macro ──────────────────────────────────────────────── */

#define MATCH(filter, topic) ove_mqtt_topic_matches(filter, strlen(filter), topic, strlen(topic))

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

/* '#' is a wildcard only as the final char after '/' (or as the whole
 * filter).  Malformed placements must not match every topic. */
static void test_hash_malformed_not_wildcard(void **state)
{
	(void)state;
	assert_false(MATCH("a#b", "axyz"));	 /* '#' mid-string */
	assert_false(MATCH("a#", "axyz"));	 /* '#' not preceded by '/' */
	assert_false(MATCH("sport#", "sportX")); /* ditto */
	/* Sanity: the legitimate forms still match. */
	assert_true(MATCH("#", "a/b/c"));
	assert_true(MATCH("sport/#", "sport/x"));
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
		cmocka_unit_test(test_hash_malformed_not_wildcard),
		cmocka_unit_test(test_combined),
		cmocka_unit_test(test_empty_filter),
		cmocka_unit_test(test_trailing_slash),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
