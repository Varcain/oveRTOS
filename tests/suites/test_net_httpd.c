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

	/* SHA-1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709 */
	assert_int_equal(hash[0], 0xda);
	assert_int_equal(hash[1], 0x39);
	assert_int_equal(hash[19], 0x09);
}

static void test_sha1_abc(void **state)
{
	(void)state;
	uint8_t hash[20];
	ove_sha1("abc", 3, hash);

	/* SHA-1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d */
	assert_int_equal(hash[0], 0xa9);
	assert_int_equal(hash[1], 0x99);
	assert_int_equal(hash[2], 0x3e);
	assert_int_equal(hash[19], 0x9d);
}

static void test_sha1_rfc6455(void **state)
{
	(void)state;
	/* RFC 6455 Section 4.2.2 example */
	const char *input =
		"dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
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
		cmocka_unit_test(test_sha1_empty),
		cmocka_unit_test(test_sha1_abc),
		cmocka_unit_test(test_sha1_rfc6455),
		cmocka_unit_test(test_base64_empty),
		cmocka_unit_test(test_base64_single_byte),
		cmocka_unit_test(test_base64_two_bytes),
		cmocka_unit_test(test_base64_three_bytes),
		cmocka_unit_test(test_base64_overflow),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
