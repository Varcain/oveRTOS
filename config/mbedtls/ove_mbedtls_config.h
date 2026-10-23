/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS overrides on top of mbedTLS's default config (MBEDTLS_USER_CONFIG_FILE).
 * The defaults assume a Unix or Windows host; these boards have neither.
 */

#ifndef OVE_MBEDTLS_CONFIG_H
#define OVE_MBEDTLS_CONFIG_H

/* Entropy comes from the board (ove_hal_entropy_fill) through
 * mbedtls_hardware_poll() in backends/common/ove_net_tls.c. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* Certificate dates are checked against the SNTP clock (ove_net_tls.c). A
 * compile-time mapping, so nothing references the C library's time(), which
 * these boards cannot back. */
#include <time.h>
time_t ove_mbedtls_time(time_t *timer);
#define MBEDTLS_PLATFORM_TIME_MACRO ove_mbedtls_time
/* Millisecond clock for TLS timeouts: ove_time_get_us() (ove_net_tls.c). */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* BSD sockets, timers and files of a hosted OS: ove_net_tls.c brings its own
 * socket I/O, and nothing here persists keys. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* Outgoing records of 4 KB: the clients send requests and packets, which
 * the TLS send loops split, and the session then needs ~23 KB less memory
 * (STM32F746, TLS 1.3, P-256: 57 KB peak with 16 KB records, 34 KB with
 * 4 KB).  Incoming records stay 16 KB: servers may send full-size records,
 * and the max_fragment_length extension asking them not to is often
 * ignored.  A client certificate chain must fit one outgoing record.
 * CONFIG_OVE_NET_TLS_OUT_CONTENT_LEN sets it (cmake/OveMbedtls.cmake). */
#ifndef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#endif

#endif /* OVE_MBEDTLS_CONFIG_H */
