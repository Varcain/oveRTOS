/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * C side of the Rust and Zig call round trips. The layout checks run at
 * compile time; these show that values cross the call intact at run time.
 */
#include "ove/ove.h"

/* Bit N is set when field N holds the value every binding writes. */
unsigned int ove_test_abi_spi_cfg(const struct ove_spi_cfg *cfg)
{
	return (cfg->instance == 5u) | (cfg->clock_hz == 12345678u) << 1 |
	       (cfg->mode == OVE_SPI_MODE_3) << 2 | (cfg->bit_order == OVE_SPI_LSB_FIRST) << 3 |
	       (cfg->word_size == 16u) << 4;
}

/* Returns the key only if all 64 bits arrived, so the caller sees both ways. */
ove_irq_key_t ove_test_abi_irq_key(ove_irq_key_t key)
{
	return key == 0x1122334455667788u ? key : 0;
}
