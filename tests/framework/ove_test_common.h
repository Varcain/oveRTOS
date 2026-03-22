/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_TEST_COMMON_H
#define OVE_TEST_COMMON_H

/* Timing tolerance for delay/sleep accuracy tests (milliseconds) */
#define OVE_TEST_TIMING_TOLERANCE_MS  50

/* Assert that a duration is within tolerance of an expected value */
#define assert_duration_within(actual_us, expected_ms, tolerance_ms) \
    do { \
        int64_t _actual_ms = (int64_t)(actual_us) / 1000; \
        int64_t _expected = (int64_t)(expected_ms); \
        int64_t _tol = (int64_t)(tolerance_ms); \
        assert_true(_actual_ms >= _expected - _tol); \
        assert_true(_actual_ms <= _expected + _tol); \
    } while (0)

#endif /* OVE_TEST_COMMON_H */
