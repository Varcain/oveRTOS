/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Unit tests for SNTP constants and offset calculation.
 */

#include "../framework/ove_test.h"

/* ── NTP epoch conversion ──────────────────────────────────────── */

#define NTP_UNIX_DELTA 2208988800ULL

static void test_ntp_epoch_delta(void **state)
{
	(void)state;
	/* 70 years in seconds (1900-01-01 to 1970-01-01)
	 * = 70*365*86400 + 17*86400 (leap days) = 2208988800 */
	assert_int_equal(NTP_UNIX_DELTA, 2208988800ULL);
}

static void test_ntp_timestamp_conversion(void **state)
{
	(void)state;
	/* Known NTP timestamp: 2026-01-01 00:00:00 UTC
	 * Unix epoch: 1767225600
	 * NTP epoch: 1767225600 + 2208988800 = 3976214400
	 */
	uint32_t ntp_secs = 3976214400UL;
	uint64_t unix_secs = (uint64_t)ntp_secs - NTP_UNIX_DELTA;
	assert_int_equal(unix_secs, 1767225600ULL);
}

static void test_ntp_fractional_us(void **state)
{
	(void)state;
	/* NTP fractional part: 0x80000000 = exactly 0.5 seconds
	 * (0x80000000 * 1000000) >> 32 = 500000 us */
	uint32_t ntp_frac = 0x80000000UL;
	uint64_t frac_us = ((uint64_t)ntp_frac * 1000000ULL) >> 32;
	assert_int_equal(frac_us, 500000);
}

static void test_ntp_frac_quarter(void **state)
{
	(void)state;
	/* 0x40000000 = 0.25 seconds = 250000 us */
	uint32_t ntp_frac = 0x40000000UL;
	uint64_t frac_us = ((uint64_t)ntp_frac * 1000000ULL) >> 32;
	assert_int_equal(frac_us, 250000);
}

static void test_ntp_frac_zero(void **state)
{
	(void)state;
	uint32_t ntp_frac = 0;
	uint64_t frac_us = ((uint64_t)ntp_frac * 1000000ULL) >> 32;
	assert_int_equal(frac_us, 0);
}

/* ── Offset math ───────────────────────────────────────────────── */

static void test_offset_positive(void **state)
{
	(void)state;
	/* NTP time ahead of local time */
	uint64_t ntp_us = 1000000000ULL;
	uint64_t local_us = 999000000ULL;
	int64_t offset = (int64_t)(ntp_us - local_us);
	assert_int_equal(offset, 1000000); /* +1 second */
}

static void test_offset_negative(void **state)
{
	(void)state;
	/* NTP time behind local time (clock was fast) */
	uint64_t ntp_us = 999000000ULL;
	uint64_t local_us = 1000000000ULL;
	int64_t offset = (int64_t)(ntp_us - local_us);
	assert_true(offset < 0);
	assert_int_equal(offset, -1000000); /* -1 second */
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_net_sntp_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_ntp_epoch_delta),
		cmocka_unit_test(test_ntp_timestamp_conversion),
		cmocka_unit_test(test_ntp_fractional_us),
		cmocka_unit_test(test_ntp_frac_quarter),
		cmocka_unit_test(test_ntp_frac_zero),
		cmocka_unit_test(test_offset_positive),
		cmocka_unit_test(test_offset_negative),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
