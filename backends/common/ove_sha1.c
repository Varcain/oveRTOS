/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Minimal SHA-1 (RFC 3174) for WebSocket handshake.
 */

#include "ove_sha1.h"
#include <string.h>

static uint32_t rotl32(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
	uint32_t w[80];
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4 + 0] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
	}
	for (i = 16; i < 80; i++)
		w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	uint32_t a = state[0], b = state[1], c = state[2];
	uint32_t d = state[3], e = state[4];

	for (i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | ((~b) & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		uint32_t t = rotl32(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rotl32(b, 30);
		b = a;
		a = t;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

void ove_sha1(const void *data, size_t len, uint8_t out[20])
{
	uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

	const uint8_t *p = (const uint8_t *)data;
	size_t remaining = len;

	/* Process full 64-byte blocks */
	while (remaining >= 64) {
		sha1_transform(state, p);
		p += 64;
		remaining -= 64;
	}

	/* Pad final block */
	uint8_t block[64];
	memset(block, 0, sizeof(block));
	memcpy(block, p, remaining);
	block[remaining] = 0x80;

	if (remaining >= 56) {
		sha1_transform(state, block);
		memset(block, 0, sizeof(block));
	}

	/* Append bit length (big-endian 64-bit) */
	uint64_t bits = (uint64_t)len * 8;
	block[56] = (uint8_t)(bits >> 56);
	block[57] = (uint8_t)(bits >> 48);
	block[58] = (uint8_t)(bits >> 40);
	block[59] = (uint8_t)(bits >> 32);
	block[60] = (uint8_t)(bits >> 24);
	block[61] = (uint8_t)(bits >> 16);
	block[62] = (uint8_t)(bits >> 8);
	block[63] = (uint8_t)(bits);
	sha1_transform(state, block);

	/* Output big-endian */
	for (int i = 0; i < 5; i++) {
		out[i * 4 + 0] = (uint8_t)(state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(state[i]);
	}
}
