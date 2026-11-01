/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_TEST_SPI_NUTTX_TRANSFER_H
#define OVE_TEST_SPI_NUTTX_TRANSFER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SPIIOC_TRANSFER 0x1234ul
#define SPIDEV_USER(index) (index)
/* Dependency-double declarations follow the pinned native API: nwords is
 * size_t, and nbits chooses the bytes consumed by each native word. */
struct spi_trans_s {
	bool deselect;
	unsigned int delay;
	size_t nwords;
	const void *txbuffer;
	void *rxbuffer;
};
struct spi_sequence_s {
	uint32_t dev;
	uint8_t mode, nbits, ntrans;
	uint32_t frequency;
	struct spi_trans_s *trans;
};
#endif
