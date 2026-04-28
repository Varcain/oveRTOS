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

#ifndef OVE_BASE64_H
#define OVE_BASE64_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Base64-encode a buffer.
 *
 * @param[in]  src     Input data.
 * @param[in]  src_len Input length in bytes.
 * @param[out] dst     Output buffer (must be at least 4*ceil(src_len/3)+1 bytes).
 * @param[in]  dst_len Output buffer size.
 * @return Number of characters written (excluding NUL), or -1 on overflow.
 */
int ove_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif /* OVE_BASE64_H */
