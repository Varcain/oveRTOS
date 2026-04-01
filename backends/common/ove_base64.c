/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Minimal Base64 encoder for WebSocket handshake.
 */

#include "ove_base64.h"

static const char b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int ove_base64_encode(const uint8_t *src, size_t src_len,
		      char *dst, size_t dst_len)
{
	size_t out_len = 4 * ((src_len + 2) / 3);

	if (dst_len < out_len + 1)
		return -1;

	size_t i = 0;
	size_t j = 0;

	while (i + 2 < src_len) {
		uint32_t v = ((uint32_t)src[i] << 16) |
			     ((uint32_t)src[i + 1] << 8) |
			     ((uint32_t)src[i + 2]);
		dst[j++] = b64[(v >> 18) & 0x3F];
		dst[j++] = b64[(v >> 12) & 0x3F];
		dst[j++] = b64[(v >>  6) & 0x3F];
		dst[j++] = b64[(v      ) & 0x3F];
		i += 3;
	}

	if (i < src_len) {
		uint32_t v = (uint32_t)src[i] << 16;
		if (i + 1 < src_len)
			v |= (uint32_t)src[i + 1] << 8;

		dst[j++] = b64[(v >> 18) & 0x3F];
		dst[j++] = b64[(v >> 12) & 0x3F];
		dst[j++] = (i + 1 < src_len) ? b64[(v >> 6) & 0x3F] : '=';
		dst[j++] = '=';
	}

	dst[j] = '\0';
	return (int)j;
}
