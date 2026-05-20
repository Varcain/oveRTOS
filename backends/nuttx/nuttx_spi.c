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

int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint64_t timeout_ns)
{
	(void)timeout_ns; /* NuttX spi_transfer ioctl has no timeout knob */

	/* nwords is uint16_t in NuttX's spi_trans_s — chunk larger transfers
	 * so we don't silently truncate. */
	const size_t MAX_CHUNK = 0xFFFF;

	const uint8_t *txp = (const uint8_t *)tx;
	uint8_t *rxp = (uint8_t *)rx;

	while (len > 0) {
		size_t chunk = len > MAX_CHUNK ? MAX_CHUNK : len;

		struct spi_sequence_s seq;
		struct spi_trans_s trans;

		memset(&seq, 0, sizeof(seq));
		memset(&trans, 0, sizeof(trans));

		seq.dev = SPIDEV_USER(0);
		seq.mode = spi->mode;
		seq.nbits = spi->word_size;
		seq.frequency = spi->clock_hz;
		seq.ntrans = 1;
		seq.trans = &trans;

		trans.deselect = (len == chunk); /* only deselect on last chunk */
		trans.nwords = (uint16_t)chunk;
		trans.txbuffer = txp;
		trans.rxbuffer = rxp;

		int ret = ioctl(spi->fd, SPIIOC_TRANSFER, (unsigned long)&seq);
		if (ret < 0)
			return OVE_ERR_BUS_ERROR;

		if (txp)
			txp += chunk;
		if (rxp)
			rxp += chunk;
		len -= chunk;
	}
	return OVE_OK;
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
