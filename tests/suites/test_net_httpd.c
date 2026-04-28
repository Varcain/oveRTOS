/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Unit tests for HTTPD internals: SHA-1, Base64, WebSocket handshake key.
 *
 * These test the pure-logic crypto helpers used by the WebSocket
 * handshake without requiring a running HTTP server or network I/O.
 */

#include "../framework/ove_test.h"
#include <string.h>

/* Include the implementations directly for testing static functions */
#include "ove_sha1.h"
#include "ove_base64.h"

/* ── SHA-1 tests ───────────────────────────────────────────────── */

static void test_sha1_empty(void **state)
{
	(void)state;
	uint8_t hash[20];
	ove_sha1("", 0, hash);

	const uint8_t expected[20] = {
		0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
		0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09,
	};
	assert_memory_equal(hash, expected, sizeof(expected));
}

static void test_sha1_abc(void **state)
{
	(void)state;
	uint8_t hash[20];
	ove_sha1("abc", 3, hash);

	const uint8_t expected[20] = {
		0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
		0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d,
	};
	assert_memory_equal(hash, expected, sizeof(expected));
}

static void test_sha1_rfc6455(void **state)
{
	(void)state;
	/* RFC 6455 Section 4.2.2 example */
	const char *input = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	uint8_t hash[20];
	ove_sha1(input, strlen(input), hash);

	char b64[32];
	ove_base64_encode(hash, 20, b64, sizeof(b64));
	assert_string_equal(b64, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

/* ── Base64 tests ──────────────────────────────────────────────── */

static void test_base64_empty(void **state)
{
	(void)state;
	char out[8];
	int len = ove_base64_encode((const uint8_t *)"", 0, out, sizeof(out));
	assert_int_equal(len, 0);
	assert_string_equal(out, "");
}

static void test_base64_single_byte(void **state)
{
	(void)state;
	char out[8];
	uint8_t in = 0x41; /* 'A' */
	int len = ove_base64_encode(&in, 1, out, sizeof(out));
	assert_int_equal(len, 4);
	assert_string_equal(out, "QQ==");
}

static void test_base64_two_bytes(void **state)
{
	(void)state;
	char out[8];
	uint8_t in[2] = {0x41, 0x42}; /* "AB" */
	int len = ove_base64_encode(in, 2, out, sizeof(out));
	assert_int_equal(len, 4);
	assert_string_equal(out, "QUI=");
}

static void test_base64_three_bytes(void **state)
{
	(void)state;
	char out[8];
	uint8_t in[3] = {0x41, 0x42, 0x43}; /* "ABC" */
	int len = ove_base64_encode(in, 3, out, sizeof(out));
	assert_int_equal(len, 4);
	assert_string_equal(out, "QUJD");
}

static void test_base64_overflow(void **state)
{
	(void)state;
	char out[4]; /* too small */
	int len = ove_base64_encode((const uint8_t *)"ABCDEF", 6, out, sizeof(out));
	assert_int_equal(len, -1); /* overflow */
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_net_httpd_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_sha1_empty),	   cmocka_unit_test(test_sha1_abc),
		cmocka_unit_test(test_sha1_rfc6455),	   cmocka_unit_test(test_base64_empty),
		cmocka_unit_test(test_base64_single_byte), cmocka_unit_test(test_base64_two_bytes),
		cmocka_unit_test(test_base64_three_bytes), cmocka_unit_test(test_base64_overflow),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
