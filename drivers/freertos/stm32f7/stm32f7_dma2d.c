/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746 DMA2D (Chrom-ART) driver — the FreeRTOS ove_hal_dma2d backend. The
 * coordinator hands it a validated ove_dma2d_desc_t (all addresses already
 * bounds-checked against the guest region by /dev/dma2d) and this programs the
 * peripheral raw and polls to completion. The descriptor's colour-format values
 * equal the DMA2D xPFCCR.CM field by design (see lxp_uapi.h); mode is a small
 * lookup. D-cache runs ON for the personality, so sources + output are cleaned
 * before the transfer (fresh SDRAM for the engine, and dirty CPU lines can't
 * evict over the result) and the output is invalidated after (fresh for the CPU).
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_DEV_DMA2D)

#include "ove/hal/hal_dma2d.h"
#include "ove/types.h"
#include "stm32f7xx_hal.h" /* CMSIS: DMA2D / RCC / SCB */

#include <stdint.h>

/* bytes/pixel, rounded up (matches the device's validation). */
static uint32_t bpp_of(uint32_t cf)
{
	switch (cf) {
	case 0:
		return 4; /* ARGB8888 */
	case 1:
		return 3; /* RGB888 */
	case 2:
	case 3:
	case 4:
		return 2; /* RGB565 / ARGB1555 / ARGB4444 */
	default:
		return 1; /* A8 / A4 */
	}
}

/* Cache op over the exact bytes a plane touches, 32-byte-expanded at both ends. */
static void cache_plane(uintptr_t addr, uint32_t w, uint32_t h, uint32_t off, uint32_t cf, int inv)
{
	uint32_t bpp = bpp_of(cf);
	uint32_t span = ((h - 1u) * (w + off) + w) * bpp;
	uintptr_t start = addr & ~(uintptr_t)31u;
	uintptr_t end = ((addr + span - 1u) & ~(uintptr_t)31u) + 32u;
	if (inv)
		SCB_InvalidateDCache_by_Addr((void *)start, (int32_t)(end - start));
	else
		SCB_CleanDCache_by_Addr((void *)start, (int32_t)(end - start));
}

int ove_hal_dma2d_init(void)
{
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2DEN; /* idempotent; already on via BSP LCD */
	__DSB();
	return OVE_OK;
}

int ove_hal_dma2d_submit(const ove_dma2d_desc_t *op)
{
	/* ABI mode → DMA2D CR.MODE (M2M / M2M+PFC / M2M+blend / blend-fixed-fg / R2M). */
	static const uint32_t cr_mode[5] = {0u, 1u, 2u, 2u, 3u};
	if (!op || op->mode > 4u || op->w == 0u || op->h == 0u)
		return OVE_ERR_INVALID_PARAM;
	int is_r2m = (op->mode == 4u);
	int is_blend = (op->mode == 2u || op->mode == 3u);
	int dc = (SCB->CCR & SCB_CCR_DC_Msk) != 0;

	if (dc) {
		if (!is_r2m)
			cache_plane(op->fg_addr, op->w, op->h, op->fg_offset, op->fg_cf, 0);
		if (is_blend)
			cache_plane(op->bg_addr, op->w, op->h, op->bg_offset, op->bg_cf, 0);
		cache_plane(op->out_addr, op->w, op->h, op->out_offset, op->out_cf, 0);
	}

	DMA2D->CR = cr_mode[op->mode] << 16; /* MODE, START cleared */
	DMA2D->OPFCCR = op->out_cf & 0x7u;
	DMA2D->OCOLR = op->out_color;
	DMA2D->OMAR = (uint32_t)op->out_addr;
	DMA2D->OOR = op->out_offset;
	DMA2D->NLR = (op->w << 16) | op->h;
	if (!is_r2m) {
		DMA2D->FGMAR = (uint32_t)op->fg_addr;
		DMA2D->FGOR = op->fg_offset;
		DMA2D->FGPFCCR = (op->fg_cf & 0xfu) | (op->fg_alpha_mode << 16) | (op->fg_alpha << 24);
		DMA2D->FGCOLR = op->fg_color;
	}
	if (is_blend) {
		DMA2D->BGMAR = (uint32_t)op->bg_addr;
		DMA2D->BGOR = op->bg_offset;
		DMA2D->BGPFCCR = (op->bg_cf & 0xfu) | (op->bg_alpha_mode << 16) | (op->bg_alpha << 24);
		DMA2D->BGCOLR = op->bg_color;
	}

	DMA2D->IFCR = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCTCIF | DMA2D_IFCR_CCEIF;
	__DSB();
	DMA2D->CR |= DMA2D_CR_START; /* HW clears START on completion */

	uint32_t spins = 0;
	while (DMA2D->CR & DMA2D_CR_START) {
		if (++spins > 4000000u)
			return OVE_ERR_TIMEOUT;
	}
	if (DMA2D->ISR & DMA2D_ISR_TEIF)
		return OVE_ERR_BUS_ERROR; /* transfer / configuration error */

	if (dc)
		cache_plane(op->out_addr, op->w, op->h, op->out_offset, op->out_cf, 1);
	return OVE_OK;
}

/* Coordinator-side self-test: solid-fill an 8x8 RGB565 scratch rect and read it
 * back. Proves the register path + cache coherency end to end before any guest
 * uses it. Returns OVE_OK / negative. */
int ove_hal_dma2d_selftest(void);
int ove_hal_dma2d_selftest(void)
{
	static uint16_t buf[64] __attribute__((aligned(32)));
	for (int i = 0; i < 64; i++)
		buf[i] = 0xABCD; /* sentinel (dirties the cache lines) */
	ove_dma2d_desc_t d = {0};
	d.mode = 4u; /* R2M fill */
	d.w = 8u;
	d.h = 8u;
	d.out_addr = (uintptr_t)buf;
	d.out_cf = 2u;		 /* RGB565 */
	d.out_color = 0xF800u;	 /* red */
	int r = ove_hal_dma2d_submit(&d);
	if (r != OVE_OK)
		return r;
	for (int i = 0; i < 64; i++)
		if (buf[i] != 0xF800u)
			return OVE_ERR_BUS_ERROR;
	return OVE_OK;
}

#endif /* CONFIG_OVE_LINUX_DEV_DMA2D */
