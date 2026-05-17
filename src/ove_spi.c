/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_SPI

#include "ove/spi.h"
#include "ove/hal/hal_spi.h"
#include "ove/sync.h"
#include "ove/gpio.h"
#include "ove_backend_common.h"

#include <string.h>

/* ── CS helpers ──────────────────────────────────────────────────── */

static void cs_assert(const struct ove_spi_cs *cs)
{
	if (cs == NULL)
		return;
	ove_gpio_set(cs->gpio_port, cs->gpio_pin, cs->active_low ? 0 : 1);
}

static void cs_deassert(const struct ove_spi_cs *cs)
{
	if (cs == NULL)
		return;
	ove_gpio_set(cs->gpio_port, cs->gpio_pin, cs->active_low ? 1 : 0);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

int ove_spi_init(ove_spi_t *spi, ove_spi_storage_t *storage, const struct ove_spi_cfg *cfg)
{
	int ret;

	if (spi == NULL || storage == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->word_size != 8 && cfg->word_size != 16)
		return OVE_ERR_INVALID_PARAM;

	storage->instance = cfg->instance;
	storage->clock_hz = cfg->clock_hz;
	storage->mode = (uint8_t)cfg->mode;
	storage->bit_order = (uint8_t)cfg->bit_order;
	storage->word_size = cfg->word_size;
	*spi = storage;

	ret = ove_mutex_init(&storage->bus_mtx, &storage->bus_mtx_storage);
	if (ret != OVE_OK)
		return ret;

	ret = ove_hal_spi_open(*spi, cfg);
	if (ret != OVE_OK) {
		ove_mutex_deinit(storage->bus_mtx);
		return ret;
	}

	return OVE_OK;
}

void ove_spi_deinit(ove_spi_t spi)
{
	if (spi == NULL)
		return;
	ove_hal_spi_close(spi);
	ove_mutex_deinit(spi->bus_mtx);
}

#ifdef OVE_HEAP_SPI
int ove_spi_create(ove_spi_t *spi, const struct ove_spi_cfg *cfg)
{
	ove_spi_storage_t *storage;

	if (spi == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;

	storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (storage == NULL)
		return OVE_ERR_NO_MEMORY;

	memset(storage, 0, sizeof(*storage));

	int ret = ove_spi_init(spi, storage, cfg);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(storage);
		return ret;
	}
	return OVE_OK;
}

void ove_spi_destroy(ove_spi_t spi)
{
	if (spi == NULL)
		return;
	ove_spi_deinit(spi);
	OVE_BACKEND_FREE(spi);
}
#endif /* OVE_HEAP_SPI */

/* ── Operations ──────────────────────────────────────────────────── */

int ove_spi_transfer(ove_spi_t spi, const struct ove_spi_cs *cs, const void *tx, void *rx,
		     size_t len, uint64_t timeout_ns)
{
	int ret;

	if (spi == NULL || len == 0)
		return OVE_ERR_INVALID_PARAM;
	if (tx == NULL && rx == NULL)
		return OVE_ERR_INVALID_PARAM;

	OVE_LOCK_INFINITE(spi->bus_mtx);
	cs_assert(cs);

	ret = ove_hal_spi_transfer(spi, tx, rx, len, timeout_ns);

	cs_deassert(cs);
	ove_mutex_unlock(spi->bus_mtx);

	return ret;
}

int ove_spi_write(ove_spi_t spi, const struct ove_spi_cs *cs, const void *data, size_t len,
		  uint64_t timeout_ns)
{
	return ove_spi_transfer(spi, cs, data, NULL, len, timeout_ns);
}

int ove_spi_read(ove_spi_t spi, const struct ove_spi_cs *cs, void *buf, size_t len,
		 uint64_t timeout_ns)
{
	return ove_spi_transfer(spi, cs, NULL, buf, len, timeout_ns);
}

int ove_spi_transfer_seq(ove_spi_t spi, const struct ove_spi_cs *cs,
			 const struct ove_spi_xfer *xfers, unsigned int num_xfers,
			 uint64_t timeout_ns)
{
	unsigned int i;
	int ret = OVE_OK;

	if (spi == NULL || xfers == NULL || num_xfers == 0)
		return OVE_ERR_INVALID_PARAM;

	OVE_LOCK_INFINITE(spi->bus_mtx);
	cs_assert(cs);

	for (i = 0; i < num_xfers; i++) {
		if (xfers[i].len == 0)
			continue;
		ret = ove_hal_spi_transfer(spi, xfers[i].tx, xfers[i].rx, xfers[i].len, timeout_ns);
		if (ret != OVE_OK)
			break;
	}

	cs_deassert(cs);
	ove_mutex_unlock(spi->bus_mtx);

	return ret;
}

#endif /* CONFIG_OVE_SPI */
