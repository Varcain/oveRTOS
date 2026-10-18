/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * QEMU MPS2 entropy for the TLS layer. The emulated board has no RNG, so this
 * is a predictable generator seeded from the boot clock: enough to exercise
 * TLS under QEMU, worthless as a secret. It says so on first use.
 */

#include "ove/hal/hal_entropy.h"
#include "ove/log.h"
#include "ove/time.h"
#include "ove/types.h"

#include <stdint.h>

int ove_hal_entropy_fill(void *buf, size_t len)
{
	static uint64_t state;
	uint8_t *out = buf;

	if (state == 0) {
		uint64_t now = 0;
		ove_time_get_us(&now);
		state = now ^ 0x6f76655f746c7321ULL;
		OVE_LOG_WRN("entropy: QEMU test-only source, not for real secrets");
	}
	for (size_t i = 0; i < len; i++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		out[i] = (uint8_t)(state >> 56);
	}
	return OVE_OK;
}
