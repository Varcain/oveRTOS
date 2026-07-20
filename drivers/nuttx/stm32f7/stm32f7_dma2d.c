/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746 DMA2D (Chrom-ART) driver — the NuttX ove_hal_dma2d backend. Same
 * register sequence as the FreeRTOS backend, but NuttX primitives: raw register
 * pokes (no CMSIS struct on the app path), up_{clean,invalidate}_dcache for
 * coherency (no-ops when the D-cache is off), and a raw RCC clock enable. The
 * coordinator has already bounds-checked every address against the guest region.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_DEV_DMA2D)

#include "ove/hal/hal_dma2d.h"
#include "ove/types.h"

#include <nuttx/cache.h>
#include <stdint.h>

#define DMA2D_BASE 0x4002b000u
#define D2(off) (*(volatile uint32_t *)(DMA2D_BASE + (off)))
#define D2_CR 0x00u
#define D2_ISR 0x04u
#define D2_IFCR 0x08u
#define D2_FGMAR 0x0cu
#define D2_FGOR 0x10u
#define D2_BGMAR 0x14u
#define D2_BGOR 0x18u
#define D2_FGPFCCR 0x1cu
#define D2_FGCOLR 0x20u
#define D2_BGPFCCR 0x24u
#define D2_BGCOLR 0x28u
#define D2_OPFCCR 0x34u
#define D2_OCOLR 0x38u
#define D2_OMAR 0x3cu
#define D2_OOR 0x40u
#define D2_NLR 0x44u
#define CR_START (1u << 0)
#define ISR_TEIF (1u << 0) /* transfer error */
#define ISR_CEIF (1u << 5) /* configuration error */
#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830u)
#define AHB1ENR_DMA2DEN (1u << 23)

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

static void cache_plane(uintptr_t addr, uint32_t w, uint32_t h, uint32_t off, uint32_t cf, int inv)
{
	uint32_t bpp = bpp_of(cf);
	uint32_t span = ((h - 1u) * (w + off) + w) * bpp;
	uintptr_t start = addr & ~(uintptr_t)31u;
	uintptr_t end = ((addr + span - 1u) & ~(uintptr_t)31u) + 32u;
	if (inv)
		up_invalidate_dcache(start, end);
	else
		up_clean_dcache(start, end);
}

int ove_hal_dma2d_init(void)
{
	RCC_AHB1ENR |= AHB1ENR_DMA2DEN;
	__asm__ volatile("dsb 0xf" ::: "memory");
	return OVE_OK;
}

int ove_hal_dma2d_submit(const ove_dma2d_desc_t *op)
{
	static const uint32_t cr_mode[5] = {0u, 1u, 2u, 2u, 3u};
	if (!op || op->mode > 4u || op->w == 0u || op->h == 0u)
		return OVE_ERR_INVALID_PARAM;
	int is_r2m = (op->mode == 4u);
	int is_blend = (op->mode == 2u || op->mode == 3u);

	/* Clean sources + output so DMA2D reads fresh SDRAM and no dirty CPU line can
	 * evict over the result (up_* are no-ops when the D-cache is off). */
	if (!is_r2m)
		cache_plane(op->fg_addr, op->w, op->h, op->fg_offset, op->fg_cf, 0);
	if (is_blend)
		cache_plane(op->bg_addr, op->w, op->h, op->bg_offset, op->bg_cf, 0);
	cache_plane(op->out_addr, op->w, op->h, op->out_offset, op->out_cf, 0);

	D2(D2_CR) = cr_mode[op->mode] << 16;
	D2(D2_OPFCCR) = op->out_cf & 0x7u;
	D2(D2_OCOLR) = op->out_color;
	D2(D2_OMAR) = (uint32_t)op->out_addr;
	D2(D2_OOR) = op->out_offset;
	D2(D2_NLR) = (op->w << 16) | op->h;
	if (!is_r2m) {
		D2(D2_FGMAR) = (uint32_t)op->fg_addr;
		D2(D2_FGOR) = op->fg_offset;
		D2(D2_FGPFCCR) = (op->fg_cf & 0xfu) | (op->fg_alpha_mode << 16) | (op->fg_alpha << 24);
		D2(D2_FGCOLR) = op->fg_color;
	}
	if (is_blend) {
		D2(D2_BGMAR) = (uint32_t)op->bg_addr;
		D2(D2_BGOR) = op->bg_offset;
		D2(D2_BGPFCCR) = (op->bg_cf & 0xfu) | (op->bg_alpha_mode << 16) | (op->bg_alpha << 24);
		D2(D2_BGCOLR) = op->bg_color;
	}
	D2(D2_IFCR) = 0x3fu; /* clear all interrupt flags */
	__asm__ volatile("dsb 0xf" ::: "memory");
	D2(D2_CR) |= CR_START; /* HW clears START on completion */

	uint32_t spins = 0;
	while (D2(D2_CR) & CR_START) {
		if (++spins > 4000000u)
			return OVE_ERR_TIMEOUT;
	}
	if (D2(D2_ISR) & (ISR_TEIF | ISR_CEIF))
		return OVE_ERR_BUS_ERROR;

	cache_plane(op->out_addr, op->w, op->h, op->out_offset, op->out_cf, 1);
	return OVE_OK;
}

int ove_hal_dma2d_selftest(void)
{
	static uint16_t buf[64] __attribute__((aligned(32)));
	for (int i = 0; i < 64; i++)
		buf[i] = 0xABCD; /* sentinel */
	ove_dma2d_desc_t d = {0};
	d.mode = 4u; /* R2M fill */
	d.w = 8u;
	d.h = 8u;
	d.out_addr = (uintptr_t)buf;
	d.out_cf = 2u;	       /* RGB565 */
	d.out_color = 0xF800u; /* red */
	int r = ove_hal_dma2d_submit(&d);
	if (r != OVE_OK)
		return r;
	for (int i = 0; i < 64; i++)
		if (buf[i] != 0xF800u)
			return OVE_ERR_BUS_ERROR;
	return OVE_OK;
}

#endif /* CONFIG_OVE_LINUX_DEV_DMA2D */
