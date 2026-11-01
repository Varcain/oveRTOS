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
#include <zephyr/drivers/spi.h>

#ifdef CONFIG_OVE_ASYNC
static void zephyr_spi_async_work_handler(struct k_work *work);
#endif

static const struct device *instance_to_dev(unsigned int instance)
{
	switch (instance) {
	case 0:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi1));
	case 1:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi2));
	case 2:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi3));
	case 3:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi4));
	default:
		return NULL;
	}
}

int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg)
{
	const struct device *dev = instance_to_dev(cfg->instance);

	if (dev == NULL || !device_is_ready(dev))
		return OVE_ERR_INVALID_PARAM;

	spi->dev = dev;
#ifdef CONFIG_OVE_ASYNC
	k_work_init(&spi->async_work, zephyr_spi_async_work_handler);
#endif
	return OVE_OK;
}

void ove_hal_spi_close(ove_spi_t spi)
{
	spi->dev = NULL;
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
	(void)timeout_ns;

	uint16_t operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(spi->word_size);

	switch (spi->mode) {
	case OVE_SPI_MODE_1:
		operation |= SPI_MODE_CPHA;
		break;
	case OVE_SPI_MODE_2:
		operation |= SPI_MODE_CPOL;
		break;
	case OVE_SPI_MODE_3:
		operation |= SPI_MODE_CPOL | SPI_MODE_CPHA;
		break;
	default:
		break;
	}

	if (spi->bit_order == OVE_SPI_LSB_FIRST)
		operation |= SPI_TRANSFER_LSB;

	struct spi_config config = {
		.frequency = spi->clock_hz,
		.operation = operation,
	};

	struct spi_buf tx_buf = {.buf = (void *)tx, .len = len};
	struct spi_buf rx_buf = {.buf = rx, .len = len};

	struct spi_buf_set tx_set = {.buffers = tx ? &tx_buf : NULL, .count = tx ? 1 : 0};
	struct spi_buf_set rx_set = {.buffers = rx ? &rx_buf : NULL, .count = rx ? 1 : 0};

	int ret = spi_transceive(spi->dev, &config, &tx_set, &rx_set);
	return (ret == 0) ? OVE_OK : OVE_ERR_BUS_ERROR;
}

#ifdef CONFIG_OVE_ASYNC

static struct k_spinlock retire_lock;
static ove_spi_t retire_head, retire_tail;
static int retire_initializing, retire_ready;
static void zephyr_spi_retire_dispatch(struct k_work *work);
static K_WORK_DEFINE(retire_work, zephyr_spi_retire_dispatch);

static void zephyr_spi_retire_retry(struct k_timer *timer)
{
	(void)timer;
	k_spinlock_key_t key = k_spin_lock(&retire_lock);
	int pending = retire_head != NULL;
	k_spin_unlock(&retire_lock, key);
	if (pending) {
		int rc = k_work_submit(&retire_work);
		(void)rc;
	}
}
static K_TIMER_DEFINE(retire_retry, zephyr_spi_retire_retry, NULL);

int ove_hal_spi_async_prepare(void)
{
	k_spinlock_key_t key = k_spin_lock(&retire_lock);
	if (retire_ready || retire_initializing) {
		int rc = retire_ready ? OVE_OK : OVE_ERR_BUSY;
		k_spin_unlock(&retire_lock, key);
		return rc;
	}
	retire_initializing = 1;
	k_spin_unlock(&retire_lock, key);
	k_timer_start(&retire_retry, K_MSEC(10), K_MSEC(10));
	key = k_spin_lock(&retire_lock);
	retire_ready = 1;
	retire_initializing = 0;
	k_spin_unlock(&retire_lock, key);
	return OVE_OK;
}

void ove_hal_spi_async_retire(ove_spi_t spi)
{
	k_spinlock_key_t key = k_spin_lock(&retire_lock);
	spi->retire_next = NULL;
	if (retire_tail)
		retire_tail->retire_next = spi;
	else
		retire_head = spi;
	retire_tail = spi;
	k_spin_unlock(&retire_lock, key);
	/* Same-queue dispatch runs only after the transfer work's kernel tail.
	 * Only this permanent work object survives in queued notifications. */
	int rc = k_work_submit(&retire_work);
	/* A plugged/draining system queue rejects notification. The stable
	 * periodic retry preserves progress after it becomes available again. */
	(void)rc;
}

static void zephyr_spi_retire_dispatch(struct k_work *work)
{
	(void)work;
	for (unsigned i = 0; i < 4; ++i) {
		k_spinlock_key_t key = k_spin_lock(&retire_lock);
		ove_spi_t spi = retire_head;
		if (spi) {
			retire_head = spi->retire_next;
			if (!retire_head)
				retire_tail = NULL;
			spi->retire_next = NULL;
		}
		k_spin_unlock(&retire_lock, key);
		if (!spi)
			return;
		ove_spi_async_retired(spi);
	}
	k_spinlock_key_t key = k_spin_lock(&retire_lock);
	int more = retire_head != NULL;
	k_spin_unlock(&retire_lock, key);
	if (more) {
		int rc = k_work_submit(&retire_work);
		(void)rc;
	}
}

/*
 * Zephyr fallback: submit the transfer to the system workqueue. Avoids
 * depending on CONFIG_SPI_ASYNC (which requires per-driver signal support)
 * and gives us the same callback-style completion across all Zephyr boards.
 */

static void zephyr_spi_async_work_handler(struct k_work *work)
{
	struct ove_spi *spi = CONTAINER_OF(work, struct ove_spi, async_work);
	int result = ove_hal_spi_transfer(spi, spi->pending_tx, spi->pending_rx, spi->pending_len,
					  OVE_WAIT_FOREVER);
	ove_spi_async_complete(spi, result);
}

int ove_hal_spi_transfer_async(ove_spi_t spi, const void *tx, void *rx, size_t len)
{
	spi->pending_tx = tx;
	spi->pending_rx = rx;
	spi->pending_len = len;
	int ret = k_work_submit(&spi->async_work);
	if (ret < 0)
		return OVE_ERR_NO_MEMORY;
	return OVE_OK;
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_SPI */
