/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_SPI

#include "ove/hal/hal_spi.h"
#include "ove_backend_common.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* NuttX SPI character driver interface */
#include <nuttx/spi/spi_transfer.h>

int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg)
{
	char path[32];

	snprintf(path, sizeof(path), "/dev/spi%u", cfg->instance);

	int fd = open(path, O_RDWR);
	if (fd < 0)
		return OVE_ERR_INVALID_PARAM;

	spi->fd = fd;
	return OVE_OK;
}

void ove_hal_spi_close(ove_spi_t spi)
{
	if (spi->fd >= 0) {
		close(spi->fd);
		spi->fd = -1;
	}
}

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx,
			 size_t len, uint32_t timeout_ms)
{
	(void)timeout_ms;

	struct spi_sequence_s seq;
	struct spi_trans_s trans;

	memset(&seq, 0, sizeof(seq));
	memset(&trans, 0, sizeof(trans));

	seq.dev     = SPIDEV_USER(0);
	seq.mode    = spi->mode;
	seq.nbits   = spi->word_size;
	seq.frequency = spi->clock_hz;
	seq.ntrans  = 1;
	seq.trans   = &trans;

	trans.deselect = true;
	trans.nwords   = len;
	trans.txbuffer = tx;
	trans.rxbuffer = rx;

	int ret = ioctl(spi->fd, SPIIOC_TRANSFER,
			(unsigned long)&seq);
	if (ret < 0)
		return OVE_ERR_BUS_ERROR;

	return OVE_OK;
}

#endif /* CONFIG_OVE_SPI */
