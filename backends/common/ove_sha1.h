/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Minimal SHA-1 (RFC 3174) for WebSocket handshake.
 * Not for cryptographic use — SHA-1 is broken for signatures.
 */

#ifndef OVE_SHA1_H
#define OVE_SHA1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute SHA-1 hash of a buffer.
 *
 * @param[in]  data Input data.
 * @param[in]  len  Input length in bytes.
 * @param[out] out  20-byte output hash.
 */
void ove_sha1(const void *data, size_t len, uint8_t out[20]);

#ifdef __cplusplus
}
#endif

#endif /* OVE_SHA1_H */
