/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_I2S

#include "ove/i2s.h"
#include "ove/hal/hal_i2s.h"
#include "ove_backend_common.h"

#include <string.h>

/* ── Lifecycle ───────────────────────────────────────────────────── */

int ove_i2s_init(ove_i2s_t *i2s, ove_i2s_storage_t *storage, void *tx_dma_buf, void *rx_dma_buf,
		 const struct ove_i2s_cfg *cfg)
{
	if (i2s == NULL || storage == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->sample_rate == 0 || cfg->dma_buf_samples == 0)
		return OVE_ERR_INVALID_PARAM;
	if ((cfg->direction & OVE_I2S_DIR_TX) && tx_dma_buf == NULL)
		return OVE_ERR_INVALID_PARAM;
	if ((cfg->direction & OVE_I2S_DIR_RX) && rx_dma_buf == NULL)
		return OVE_ERR_INVALID_PARAM;

	memset(storage, 0, sizeof(*storage));

	storage->instance = cfg->instance;
	storage->sample_rate = cfg->sample_rate;
	storage->bit_depth = cfg->bit_depth;
	storage->channels = cfg->channels;
	storage->direction = (uint8_t)cfg->direction;
	storage->dma_buf_samples = cfg->dma_buf_samples;
	storage->tx_dma_buf = tx_dma_buf;
	storage->rx_dma_buf = rx_dma_buf;
	storage->half_buf_bytes = (cfg->dma_buf_samples / 2) * (cfg->bit_depth / 8);

	*i2s = storage;

	int ret = ove_hal_i2s_open(*i2s, cfg);
	if (ret != OVE_OK)
		return ret;

	return OVE_OK;
}

void ove_i2s_deinit(ove_i2s_t i2s)
{
	if (i2s == NULL)
		return;
	ove_hal_i2s_close(i2s);
}

#ifdef OVE_HEAP_I2S
int ove_i2s_create(ove_i2s_t *i2s, const struct ove_i2s_cfg *cfg)
{
	ove_i2s_storage_t *storage;
	void *tx_buf = NULL, *rx_buf = NULL;
	size_t buf_bytes;

	if (i2s == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;

	buf_bytes = cfg->dma_buf_samples * (cfg->bit_depth / 8);

	storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (storage == NULL)
		return OVE_ERR_NO_MEMORY;

	if (cfg->direction & OVE_I2S_DIR_TX) {
		tx_buf = OVE_BACKEND_MALLOC(buf_bytes);
		if (tx_buf == NULL) {
			OVE_BACKEND_FREE(storage);
			return OVE_ERR_NO_MEMORY;
		}
	}
	if (cfg->direction & OVE_I2S_DIR_RX) {
		rx_buf = OVE_BACKEND_MALLOC(buf_bytes);
		if (rx_buf == NULL) {
			OVE_BACKEND_FREE(tx_buf);
			OVE_BACKEND_FREE(storage);
			return OVE_ERR_NO_MEMORY;
		}
	}

	int ret = ove_i2s_init(i2s, storage, tx_buf, rx_buf, cfg);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(rx_buf);
		OVE_BACKEND_FREE(tx_buf);
		OVE_BACKEND_FREE(storage);
		return ret;
	}
	return OVE_OK;
}

void ove_i2s_destroy(ove_i2s_t i2s)
{
	void *tx, *rx;
	if (i2s == NULL)
		return;
	tx = i2s->tx_dma_buf;
	rx = i2s->rx_dma_buf;
	ove_i2s_deinit(i2s);
	OVE_BACKEND_FREE(rx);
	OVE_BACKEND_FREE(tx);
	OVE_BACKEND_FREE(i2s);
}
#endif /* OVE_HEAP_I2S */

/* ── Callbacks ───────────────────────────────────────────────────── */

int ove_i2s_set_rx_callback(ove_i2s_t i2s, ove_i2s_cb_t cb, void *user_data)
{
	if (i2s == NULL)
		return OVE_ERR_INVALID_PARAM;
	i2s->rx_cb = cb;
	i2s->rx_cb_user_data = user_data;
	return OVE_OK;
}

int ove_i2s_set_tx_callback(ove_i2s_t i2s, ove_i2s_cb_t cb, void *user_data)
{
	if (i2s == NULL)
		return OVE_ERR_INVALID_PARAM;
	i2s->tx_cb = cb;
	i2s->tx_cb_user_data = user_data;
	return OVE_OK;
}

/* ── Stream control ──────────────────────────────────────────────── */

int ove_i2s_start(ove_i2s_t i2s)
{
	if (i2s == NULL)
		return OVE_ERR_INVALID_PARAM;
	return ove_hal_i2s_start(i2s);
}

int ove_i2s_stop(ove_i2s_t i2s)
{
	if (i2s == NULL)
		return OVE_ERR_INVALID_PARAM;
	return ove_hal_i2s_stop(i2s);
}

int ove_i2s_pause(ove_i2s_t i2s)
{
	if (i2s == NULL)
		return OVE_ERR_INVALID_PARAM;
	return ove_hal_i2s_pause(i2s);
}

int ove_i2s_resume(ove_i2s_t i2s)
{
	if (i2s == NULL)
		return OVE_ERR_INVALID_PARAM;
	return ove_hal_i2s_resume(i2s);
}

/* ── Buffer access ───────────────────────────────────────────────── */

void *ove_i2s_rx_buf(ove_i2s_t i2s)
{
	if (i2s == NULL || i2s->rx_dma_buf == NULL)
		return NULL;
	if (i2s->rx_completed_half == 0)
		return i2s->rx_dma_buf;
	return (uint8_t *)i2s->rx_dma_buf + i2s->half_buf_bytes;
}

void *ove_i2s_tx_buf(ove_i2s_t i2s)
{
	if (i2s == NULL || i2s->tx_dma_buf == NULL)
		return NULL;
	if (i2s->tx_completed_half == 0)
		return i2s->tx_dma_buf;
	return (uint8_t *)i2s->tx_dma_buf + i2s->half_buf_bytes;
}

size_t ove_i2s_half_buf_size(ove_i2s_t i2s)
{
	if (i2s == NULL)
		return 0;
	return i2s->half_buf_bytes;
}

/* ── ISR helpers (called by backend DMA callbacks) ───────────────── */

void ove_i2s_rx_half_cplt_isr(ove_i2s_t i2s)
{
	i2s->rx_completed_half = 0;
	if (i2s->rx_cb)
		i2s->rx_cb(i2s, i2s->rx_cb_user_data);
}

void ove_i2s_rx_cplt_isr(ove_i2s_t i2s)
{
	i2s->rx_completed_half = 1;
	if (i2s->rx_cb)
		i2s->rx_cb(i2s, i2s->rx_cb_user_data);
}

void ove_i2s_tx_half_cplt_isr(ove_i2s_t i2s)
{
	i2s->tx_completed_half = 0;
	if (i2s->tx_cb)
		i2s->tx_cb(i2s, i2s->tx_cb_user_data);
}

void ove_i2s_tx_cplt_isr(ove_i2s_t i2s)
{
	i2s->tx_completed_half = 1;
	if (i2s->tx_cb)
		i2s->tx_cb(i2s, i2s->tx_cb_user_data);
}

#endif /* CONFIG_OVE_I2S */
