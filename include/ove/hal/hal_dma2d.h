/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Board HAL for a 2D graphics accelerator (STM32 DMA2D / Chrom-ART). The Linux
 * personality's /dev/dma2d device reaches it only through the display adapter's
 * dma2d_submit op → ove_hal_dma2d_submit, so the personality carries no direct
 * peripheral dependency. A board with DMA2D provides strong ove_hal_dma2d_*
 * definitions (drivers/.../dma2d_port); boards without it get the weak no-op
 * fallback (backends/common/ove_hal_dma2d.c) and the device returns an error →
 * the guest renders in software.
 */

#ifndef OVE_HAL_DMA2D_H
#define OVE_HAL_DMA2D_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A single validated fill/blit/blend. Board-neutral mirror of lxp_dma2d_op_t (no
 * lxp dependency). Scalar fields are the LXP_DMA2D_* ABI enums (mode / colour
 * format / alpha mode); the board maps them to DMA2D registers. Addresses are
 * absolute and were bounds-checked against the guest region before this call. */
typedef struct ove_dma2d_desc {
	uint32_t mode, w, h;
	uintptr_t out_addr;
	uint32_t out_offset, out_cf, out_color;
	uintptr_t fg_addr;
	uint32_t fg_offset, fg_cf, fg_color, fg_alpha_mode, fg_alpha;
	uintptr_t bg_addr;
	uint32_t bg_offset, bg_cf, bg_color, bg_alpha_mode, bg_alpha;
} ove_dma2d_desc_t;

/* One-time bring-up (clock, reset). OVE_OK / OVE_ERR_*. */
int ove_hal_dma2d_init(void);

/* Execute one descriptor synchronously (program the engine, wait for completion,
 * own any cache coherency). OVE_OK / OVE_ERR_*. */
int ove_hal_dma2d_submit(const ove_dma2d_desc_t *d);

/* Coordinator-side proof: HW-fill a scratch rect and read it back (register path
 * + coherency). OVE_OK on success; OVE_ERR_NOT_SUPPORTED when the board has only
 * the weak fallback (no DMA2D backend yet). */
int ove_hal_dma2d_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* OVE_HAL_DMA2D_H */
