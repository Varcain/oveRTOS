/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Entropy for the TLS layer: NuttX's /dev/random, backed by the STM32
 * hardware RNG on the F746. A board without the device (the QEMU target)
 * gets a predictable generator, with a warning, so TLS can still be
 * exercised there.
 */

#include <nuttx/config.h>

#include "ove/hal/hal_entropy.h"
#include "ove/log.h"
#include "ove/time.h"
#include "ove/types.h"

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

static int test_only_fill(uint8_t *out, size_t len)
{
	static uint64_t state;
	if (state == 0) {
		uint64_t now = 0;
		ove_time_get_us(&now);
		state = now ^ 0x6f76655f746c7321ULL;
		OVE_LOG_WRN("entropy: no /dev/random, test-only source, not for real secrets");
	}
	for (size_t i = 0; i < len; i++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		out[i] = (uint8_t)(state >> 56);
	}
	return OVE_OK;
}

int ove_hal_entropy_fill(void *buf, size_t len)
{
	uint8_t *out = buf;
	int fd = open("/dev/random", O_RDONLY);
	if (fd < 0)
		return test_only_fill(out, len);

	size_t done = 0;
	while (done < len) {
		ssize_t n = read(fd, out + done, len - done);
		if (n <= 0) {
			close(fd);
			return OVE_ERR_IO;
		}
		done += (size_t)n;
	}
	close(fd);
	return OVE_OK;
}
