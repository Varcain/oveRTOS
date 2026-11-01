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

	int length = snprintf(path, sizeof(path), "/dev/spi%u", cfg->instance);
	if (length < 0 || (size_t)length >= sizeof(path))
		return OVE_ERR_INVALID_PARAM;

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

int ove_hal_spi_validate_transfer(ove_spi_t spi, size_t len, int asynchronous)
{
	(void)spi;
	(void)len;
	(void)asynchronous;
	return OVE_OK;
}

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint64_t timeout_ns)
{
	(void)timeout_ns; /* NuttX spi_transfer ioctl has no timeout knob */

	struct spi_trans_s trans = {
		.deselect = true,
		.nwords = len / (spi->word_size == 16 ? 2 : 1),
		.txbuffer = tx,
		.rxbuffer = rx,
	};
	struct spi_sequence_s seq = {
		.dev = SPIDEV_USER(0),
		.mode = spi->mode,
		.nbits = spi->word_size,
		.frequency = spi->clock_hz,
		.ntrans = 1,
		.trans = &trans,
	};
	int ret = ioctl(spi->fd, SPIIOC_TRANSFER, (unsigned long)&seq);
	return ret < 0 ? OVE_ERR_BUS_ERROR : OVE_OK;
}

#ifdef CONFIG_OVE_ASYNC

#include <pthread.h>

static void *nuttx_spi_async_worker(void *arg)
{
	ove_spi_t spi = (ove_spi_t)arg;
	int result;

	result = ove_hal_spi_transfer(spi, spi->pending_tx, spi->pending_rx, spi->pending_len,
				      OVE_WAIT_FOREVER);
	ove_spi_async_complete(spi, result);
	return NULL;
}

int ove_hal_spi_transfer_async(ove_spi_t spi, const void *tx, void *rx, size_t len)
{
	pthread_t tid;
	pthread_attr_t attr;
	int rc;

	spi->pending_tx = tx;
	spi->pending_rx = rx;
	spi->pending_len = len;

	if (pthread_attr_init(&attr) != 0)
		return OVE_ERR_NO_MEMORY;
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	rc = pthread_create(&tid, &attr, nuttx_spi_async_worker, spi);
	pthread_attr_destroy(&attr);
	if (rc != 0)
		return OVE_ERR_NO_MEMORY;
	return OVE_OK;
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_SPI */
