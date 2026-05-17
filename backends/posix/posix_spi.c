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

#ifdef __linux__
#include <linux/spi/spidev.h>
#endif

int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg)
{
#ifdef __linux__
	char path[32];
	uint8_t mode;
	uint8_t bits;
	uint32_t speed;

	snprintf(path, sizeof(path), "/dev/spidev%u.0", cfg->instance);

	int fd = open(path, O_RDWR);
	if (fd < 0)
		return OVE_ERR_INVALID_PARAM;

	mode = (uint8_t)cfg->mode;
	if (cfg->bit_order == OVE_SPI_LSB_FIRST)
		mode |= SPI_LSB_FIRST;

	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0)
		goto err;

	bits = cfg->word_size;
	if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
		goto err;

	speed = cfg->clock_hz;
	if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
		goto err;

	spi->fd = fd;
	return OVE_OK;

err:
	close(fd);
	return OVE_ERR_NOT_SUPPORTED;
#else
	(void)cfg;
	spi->fd = -1;
	return OVE_OK;
#endif
}

void ove_hal_spi_close(ove_spi_t spi)
{
	if (spi->fd >= 0) {
		close(spi->fd);
		spi->fd = -1;
	}
}

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint64_t timeout_ns)
{
	(void)timeout_ns;

#ifdef __linux__
	struct spi_ioc_transfer xfer;

	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = (unsigned long)tx;
	xfer.rx_buf = (unsigned long)rx;
	xfer.len = (uint32_t)len;
	xfer.speed_hz = spi->clock_hz;
	xfer.bits_per_word = spi->word_size;

	if (ioctl(spi->fd, SPI_IOC_MESSAGE(1), &xfer) < 0)
		return OVE_ERR_BUS_ERROR;

	return OVE_OK;
#else
	(void)spi;
	(void)tx;
	(void)rx;
	(void)len;
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

#endif /* CONFIG_OVE_SPI */
