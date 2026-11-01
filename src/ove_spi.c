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

#ifdef CONFIG_OVE_ASYNC
enum {
	SPI_ACTIVE = 1,
	SPI_SUBMITTING = 2,
	SPI_RESULT = 4,
	SPI_CLOSING = 8,
	SPI_DESTROY = 16,
	SPI_COMPLETING = 32,
};
#endif

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
#ifdef CONFIG_OVE_ASYNC
	storage->async_busy = 0;
	storage->pending_cb = NULL;
	storage->pending_ud = NULL;
	storage->pending_cs = NULL;
	storage->pending_tx = NULL;
	storage->pending_rx = NULL;
	storage->pending_len = 0;
	storage->retire_next = NULL;
	storage->async_result = OVE_OK;
#endif
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

static void spi_deinit_now(ove_spi_t spi)
{
	ove_hal_spi_close(spi);
	ove_mutex_deinit(spi->bus_mtx);
}

#ifdef CONFIG_OVE_ASYNC
static int spi_request_close(ove_spi_t spi, int destroy)
{
	int previous = __atomic_fetch_or(
		&spi->async_busy, SPI_CLOSING | (destroy ? SPI_DESTROY : 0), __ATOMIC_ACQ_REL);
	return previous & (SPI_ACTIVE | SPI_CLOSING);
}

static void spi_finish_close(ove_spi_t spi, int flags)
{
	if (flags & SPI_CLOSING) {
		spi_deinit_now(spi);
#ifdef OVE_HEAP_SPI
		if (flags & SPI_DESTROY)
			OVE_BACKEND_FREE(spi);
#endif
	}
}
#endif

void ove_spi_deinit(ove_spi_t spi)
{
	if (spi == NULL)
		return;
#ifdef CONFIG_OVE_ASYNC
	if (spi_request_close(spi, 0))
		return;
#endif
	spi_deinit_now(spi);
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
#ifdef CONFIG_OVE_ASYNC
	if (spi_request_close(spi, 1))
		return;
#endif
	spi_deinit_now(spi);
	OVE_BACKEND_FREE(spi);
}
#endif /* OVE_HEAP_SPI */

/* ── Operations ──────────────────────────────────────────────────── */

static int spi_validate_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len,
				 int asynchronous)
{
	if (!spi || !len || (!tx && !rx))
		return OVE_ERR_INVALID_PARAM;
	if (spi->word_size != 8 && spi->word_size != 16)
		return OVE_ERR_INVALID_PARAM;
	if (spi->word_size == 16 &&
	    ((len & 1) || (uintptr_t)tx % _Alignof(uint16_t) || (uintptr_t)rx % _Alignof(uint16_t)))
		return OVE_ERR_INVALID_PARAM;
	if (tx && rx && tx != rx) {
		uintptr_t a = (uintptr_t)tx, b = (uintptr_t)rx;
		if ((a < b ? b - a : a - b) < len)
			return OVE_ERR_INVALID_PARAM;
	}
	return ove_hal_spi_validate_transfer(spi, len, asynchronous);
}

int ove_spi_transfer(ove_spi_t spi, const struct ove_spi_cs *cs, const void *tx, void *rx,
		     size_t len, uint64_t timeout_ns)
{
	int ret;

	ret = spi_validate_transfer(spi, tx, rx, len, 0);
	if (ret != OVE_OK)
		return ret;

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
	for (i = 0; i < num_xfers; i++) {
		if (!xfers[i].len)
			continue;
		ret = spi_validate_transfer(spi, xfers[i].tx, xfers[i].rx, xfers[i].len, 0);
		if (ret != OVE_OK)
			return ret;
	}

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

#ifdef CONFIG_OVE_ASYNC

int ove_spi_async_prepare(void)
{
	return ove_hal_spi_async_prepare();
}

static void spi_clear_pending(ove_spi_t spi)
{
	spi->pending_cb = NULL;
	spi->pending_ud = NULL;
	spi->pending_cs = NULL;
	spi->pending_tx = NULL;
	spi->pending_rx = NULL;
	spi->pending_len = 0;
}

int ove_spi_transfer_async(ove_spi_t spi, const struct ove_spi_cs *cs, const void *tx, void *rx,
			   size_t len, ove_dma_complete_cb cb, void *user_data)
{
	if (cb == NULL)
		return OVE_ERR_INVALID_PARAM;
	int ret = spi_validate_transfer(spi, tx, rx, len, 1);
	if (ret != OVE_OK)
		return ret;
	ret = ove_spi_async_prepare();
	if (ret != OVE_OK)
		return ret;
	int idle = 0;
	if (!__atomic_compare_exchange_n(&spi->async_busy, &idle, SPI_ACTIVE | SPI_SUBMITTING, 0,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return OVE_ERR_BUS_BUSY;
	spi->pending_cb = cb;
	spi->pending_ud = user_data;
	spi->pending_cs = cs;
	spi->pending_tx = tx;
	spi->pending_rx = rx;
	spi->pending_len = len;
	cs_assert(cs);
	ret = ove_hal_spi_transfer_async(spi, tx, rx, len);
	if (ret != OVE_OK) {
		/* Rejection retains no provider state and produces no callback. */
		cs_deassert(cs);
		spi_clear_pending(spi);
		int flags = __atomic_fetch_and(&spi->async_busy, SPI_CLOSING | SPI_DESTROY,
					       __ATOMIC_ACQ_REL);
		spi_finish_close(spi, flags);
		return ret;
	}
	/* This RMW is the submitter's last access before handing off retirement.
	 * Completion's paired RMW chooses exactly one finalizer, even when a
	 * worker/IRQ finishes before the native submission call returns. */
	int flags = __atomic_fetch_and(&spi->async_busy, ~SPI_SUBMITTING, __ATOMIC_ACQ_REL);
	if (flags & SPI_RESULT)
		ove_hal_spi_async_retire(spi);
	return ret;
}

void ove_spi_async_complete(ove_spi_t spi, int result)
{
	int flags = __atomic_load_n(&spi->async_busy, __ATOMIC_ACQUIRE);
	do {
		if (!(flags & SPI_ACTIVE) || (flags & SPI_COMPLETING))
			return;
	} while (!__atomic_compare_exchange_n(&spi->async_busy, &flags, flags | SPI_COMPLETING, 0,
					      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	cs_deassert(spi->pending_cs);
	spi->async_result = result;
	flags = __atomic_fetch_or(&spi->async_busy, SPI_RESULT, __ATOMIC_ACQ_REL);
	if (!(flags & SPI_SUBMITTING))
		ove_hal_spi_async_retire(spi);
}

void ove_spi_async_retired(ove_spi_t spi)
{
	/* Provider queues have already unlinked this object. All arguments and
	 * cleanup choices must be cached before dropping its final active state. */
	ove_dma_complete_cb cb = spi->pending_cb;
	void *user_data = spi->pending_ud;
	int result = spi->async_result;
	spi_clear_pending(spi);
	int flags =
		__atomic_fetch_and(&spi->async_busy, SPI_CLOSING | SPI_DESTROY, __ATOMIC_ACQ_REL);
	spi_finish_close(spi, flags);
	if (cb != NULL)
		cb(result, user_data);
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_SPI */
