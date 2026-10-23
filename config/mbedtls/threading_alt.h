/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * mbedTLS's mutex type for MBEDTLS_THREADING_ALT (ove_mbedtls_config.h).
 * Every mbedTLS mutex is the one recursive oveRTOS mutex in
 * backends/common/ove_net_tls.c, so an object's mutex carries nothing. No
 * oveRTOS or RTOS headers here: every mbedTLS source includes this file.
 */

#ifndef OVE_MBEDTLS_THREADING_ALT_H
#define OVE_MBEDTLS_THREADING_ALT_H

typedef struct mbedtls_threading_mutex_t {
	unsigned char ove_unused; /* a struct may not be empty */
} mbedtls_threading_mutex_t;

#endif /* OVE_MBEDTLS_THREADING_ALT_H */
