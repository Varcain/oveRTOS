/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_TEST_COMMON_H
#define OVE_TEST_COMMON_H

#include <math.h>
#include <stdint.h>

/*
 * Timing tolerance for delay/sleep accuracy tests (milliseconds).
 *
 * Per-target override: define OVE_TEST_TIMING_TOLERANCE_MS in the
 * target's ove_config.h before this header is pulled in. Native
 * stub / host Linux defaults to 50 ms; QEMU bare-metal variants
 * (slower wall-clock vs sim-time) should bump this to 150–250 ms
 * in their ove_config.h.
 */
#ifndef OVE_TEST_TIMING_TOLERANCE_MS
#define OVE_TEST_TIMING_TOLERANCE_MS  50
#endif

/* Assert that a duration is within tolerance of an expected value */
#define assert_duration_within(actual_us, expected_ms, tolerance_ms) \
    do { \
        int64_t _actual_ms = (int64_t)(actual_us) / 1000; \
        int64_t _expected = (int64_t)(expected_ms); \
        int64_t _tol = (int64_t)(tolerance_ms); \
        assert_true(_actual_ms >= _expected - _tol); \
        assert_true(_actual_ms <= _expected + _tol); \
    } while (0)

/*
 * Assert that a float/double is within `tol` of `expected`.
 * Handles NaN explicitly (NaN != NaN, so direct subtraction would pass).
 */
#define assert_float_within(actual, expected, tol) \
    do { \
        double _a = (double)(actual); \
        double _e = (double)(expected); \
        double _t = (double)(tol); \
        assert_false(isnan(_a)); \
        assert_false(isnan(_e)); \
        assert_true(fabs(_a - _e) <= _t); \
    } while (0)

#endif /* OVE_TEST_COMMON_H */
