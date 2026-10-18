/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Entropy for the TLS layer: Zephyr's CSPRNG, fed by the board's entropy
 * driver (the STM32 RNG on the F746). Boards without one (the QEMU targets)
 * only have Zephyr's non-cryptographic generator, which is used with a
 * warning so TLS can still be exercised there.
 */

#include "ove/hal/hal_entropy.h"
#include "ove/log.h"
#include "ove/types.h"

#include <zephyr/random/random.h>

int ove_hal_entropy_fill(void *buf, size_t len)
{
#if defined(CONFIG_CSPRNG_ENABLED)
	return sys_csrand_get(buf, len) == 0 ? OVE_OK : OVE_ERR_IO;
#else
	static int warned;
	if (!warned) {
		warned = 1;
		OVE_LOG_WRN("entropy: no hardware RNG, test-only source, not for real secrets");
	}
	sys_rand_get(buf, len);
	return OVE_OK;
#endif
}
