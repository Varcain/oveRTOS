/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <stdatomic.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static _Atomic int s_process_count;

static void audio_process_fn(int16_t *out, const int16_t *in,
                             unsigned int frame_count, void *user_data)
{
    (void)out;
    (void)in;
    (void)frame_count;
    (void)user_data;
    s_process_count++;
}

static struct ove_audio_config make_config(void)
{
    struct ove_audio_config cfg = {
        .sample_rate = 48000,
        .channels = 2,
        .bit_depth = 16,
        .frames_per_buffer = 256,
    };
    return cfg;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_audio_init(void **state)
{
    (void)state;
    struct ove_audio_config cfg = make_config();
    int rc = ove_audio_init(&cfg, audio_process_fn, NULL);
    assert_int_equal(rc, OVE_OK);
    ove_audio_deinit();
}

static void test_audio_start(void **state)
{
    (void)state;
    s_process_count = 0;

    struct ove_audio_config cfg = make_config();
    ove_audio_init(&cfg, audio_process_fn, NULL);

    int rc = ove_audio_start();
    assert_int_equal(rc, OVE_OK);

    test_msleep(50); /* 50 ms */

    ove_audio_stop();
    assert_true(s_process_count > 0);

    ove_audio_deinit();
}

static void test_audio_stop(void **state)
{
    (void)state;
    struct ove_audio_config cfg = make_config();
    ove_audio_init(&cfg, audio_process_fn, NULL);
    ove_audio_start();

    int rc = ove_audio_stop();
    assert_int_equal(rc, OVE_OK);

    ove_audio_deinit();
}

static void test_audio_pause(void **state)
{
    (void)state;
    struct ove_audio_config cfg = make_config();
    ove_audio_init(&cfg, audio_process_fn, NULL);
    ove_audio_start();

    int rc = ove_audio_pause();
    assert_int_equal(rc, OVE_OK);

    ove_audio_stop();
    ove_audio_deinit();
}

static void test_audio_resume(void **state)
{
    (void)state;
    struct ove_audio_config cfg = make_config();
    ove_audio_init(&cfg, audio_process_fn, NULL);
    ove_audio_start();
    ove_audio_pause();

    int rc = ove_audio_resume();
    assert_int_equal(rc, OVE_OK);

    ove_audio_stop();
    ove_audio_deinit();
}

static void test_audio_deinit(void **state)
{
    (void)state;
    struct ove_audio_config cfg = make_config();
    ove_audio_init(&cfg, audio_process_fn, NULL);

    /* Should not crash */
    ove_audio_deinit();
}

static void test_audio_start_without_init(void **state)
{
    (void)state;
    /* start without init — should not crash, may return error */
    int rc = ove_audio_start();
    (void)rc;
}

static void test_audio_stop_without_start(void **state)
{
    (void)state;
    struct ove_audio_config cfg = make_config();
    ove_audio_init(&cfg, audio_process_fn, NULL);
    /* stop without start — should not crash */
    int rc = ove_audio_stop();
    (void)rc;
    ove_audio_deinit();
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_audio_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_audio_init),
        cmocka_unit_test(test_audio_start),
        cmocka_unit_test(test_audio_stop),
        cmocka_unit_test(test_audio_pause),
        cmocka_unit_test(test_audio_resume),
        cmocka_unit_test(test_audio_deinit),
        cmocka_unit_test(test_audio_start_without_init),
        cmocka_unit_test(test_audio_stop_without_start),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
