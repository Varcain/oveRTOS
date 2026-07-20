/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Weak no-op fallback for ove_hal_dma2d on boards without a DMA2D backend, so
 * /dev/dma2d links + registers everywhere CONFIG_OVE_LINUX_DEV_DMA2D is on; the
 * device's submit then returns an error and the guest renders in software. A
 * board with Chrom-ART (e.g. drivers/freertos/stm32f7/stm32f7_dma2d.c) provides
 * strong overrides of these three.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_DEV_DMA2D)

#include "ove/hal/hal_dma2d.h"
#include "ove/types.h"

__attribute__((weak)) int ove_hal_dma2d_init(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) int ove_hal_dma2d_submit(const ove_dma2d_desc_t *d)
{
	(void)d;
	return OVE_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) int ove_hal_dma2d_selftest(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_LINUX_DEV_DMA2D */
