/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <string.h>

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_nvs_init(void **state)
{
    (void)state;
    int rc = ove_nvs_init();
    assert_int_equal(rc, OVE_OK);
    ove_nvs_deinit();
}

static void test_nvs_write_read(void **state)
{
    (void)state;
    ove_nvs_init();

    const char *key = "test_key";
    uint32_t value = 12345;
    int rc = ove_nvs_write(key, &value, sizeof(value));
    assert_int_equal(rc, OVE_OK);

    uint32_t out = 0;
    size_t out_len = 0;
    rc = ove_nvs_read(key, &out, sizeof(out), &out_len);
    assert_int_equal(rc, OVE_OK);
    assert_int_equal(out, 12345);
    assert_int_equal(out_len, sizeof(uint32_t));

    ove_nvs_deinit();
}

static void test_nvs_read_nonexistent(void **state)
{
    (void)state;
    ove_nvs_init();

    uint8_t buf[32];
    size_t len = 0;
    int rc = ove_nvs_read("no_such_key", buf, sizeof(buf), &len);
    assert_int_not_equal(rc, OVE_OK);

    ove_nvs_deinit();
}

static void test_nvs_overwrite(void **state)
{
    (void)state;
    ove_nvs_init();

    const char *key = "overwrite_key";
    uint32_t v1 = 100;
    ove_nvs_write(key, &v1, sizeof(v1));

    uint32_t v2 = 200;
    int rc = ove_nvs_write(key, &v2, sizeof(v2));
    assert_int_equal(rc, OVE_OK);

    uint32_t out = 0;
    size_t out_len = 0;
    rc = ove_nvs_read(key, &out, sizeof(out), &out_len);
    assert_int_equal(rc, OVE_OK);
    assert_int_equal(out, 200);

    ove_nvs_deinit();
}

static void test_nvs_erase_then_read(void **state)
{
    (void)state;
    ove_nvs_init();

    const char *key = "erase_key";
    uint32_t v = 42;
    ove_nvs_write(key, &v, sizeof(v));

    int rc = ove_nvs_erase(key);
    assert_int_equal(rc, OVE_OK);

    uint8_t buf[32];
    size_t len = 0;
    rc = ove_nvs_read(key, buf, sizeof(buf), &len);
    assert_int_not_equal(rc, OVE_OK);

    ove_nvs_deinit();
}

static void test_nvs_multiple_keys(void **state)
{
    (void)state;
    ove_nvs_init();

    uint32_t a = 1, b = 2, c = 3;
    ove_nvs_write("key_a", &a, sizeof(a));
    ove_nvs_write("key_b", &b, sizeof(b));
    ove_nvs_write("key_c", &c, sizeof(c));

    uint32_t out = 0;
    size_t len = 0;

    ove_nvs_read("key_a", &out, sizeof(out), &len);
    assert_int_equal(out, 1);

    len = 0;
    ove_nvs_read("key_b", &out, sizeof(out), &len);
    assert_int_equal(out, 2);

    len = 0;
    ove_nvs_read("key_c", &out, sizeof(out), &len);
    assert_int_equal(out, 3);

    ove_nvs_deinit();
}

static void test_nvs_read_small_buffer(void **state)
{
    (void)state;
    ove_nvs_init();

    const char *key = "big_data";
    uint8_t data[32];
    memset(data, 0xAA, sizeof(data));
    ove_nvs_write(key, data, sizeof(data));

    uint8_t small_buf[8];
    size_t len = 0;
    int rc = ove_nvs_read(key, small_buf, sizeof(small_buf), &len);
    /* Stub copies min(stored_len, buf_len) bytes and reports full stored length */
    assert_int_equal(rc, OVE_OK);
    assert_int_equal(len, sizeof(data));

    ove_nvs_deinit();
}

static void test_nvs_deinit_clears(void **state)
{
    (void)state;
    ove_nvs_init();

    uint32_t v = 999;
    ove_nvs_write("persist_key", &v, sizeof(v));
    ove_nvs_deinit();

    /* Re-init: all data should be gone */
    ove_nvs_init();

    uint8_t buf[32];
    size_t len = 0;
    int rc = ove_nvs_read("persist_key", buf, sizeof(buf), &len);
    assert_int_not_equal(rc, OVE_OK);

    ove_nvs_deinit();
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_nvs_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_nvs_init),
        cmocka_unit_test(test_nvs_write_read),
        cmocka_unit_test(test_nvs_read_nonexistent),
        cmocka_unit_test(test_nvs_overwrite),
        cmocka_unit_test(test_nvs_erase_then_read),
        cmocka_unit_test(test_nvs_multiple_keys),
        cmocka_unit_test(test_nvs_read_small_buffer),
        cmocka_unit_test(test_nvs_deinit_clears),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
