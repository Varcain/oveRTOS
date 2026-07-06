/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746G-Discovery NuttX board bring-up for the Linux personality's QSPI-XIP
 * rootfs + LTDC framebuffer: put the on-board N25Q128A QSPI NOR into memory-mapped
 * mode at 0x90000000 (read-only, the NOR is already programmed) so the personality
 * can XIP its rootfs from external flash, and add the FMC read-pipe delay the LTDC
 * needs. Runs from ove_hal_board_init (ove_board_init -> before ove_main / the
 * rootfs parse). Mirrors the FreeRTOS bsp.c bsp_qspi_init + bsp_sdram_fixup.
 */

#include <nuttx/config.h>
#include "ove_config.h" /* CONFIG_OVE_FB (via the app's DEV_FB select) — not in nuttx/config.h */
#include <stdint.h>
#include <nuttx/spi/qspi.h> /* qspi_meminfo_s / qspi_cmdinfo_s + QSPI_* ops + QSPIMEM_/QSPICMD_ */

#include "ove/hal/hal_board.h"
#include "ove/types.h"
#if defined(CONFIG_OVE_FB)
#include "ove/hal/hal_fb.h"
#include "ove/fb.h"
#include "board_desc.h" /* OVE_DISPLAY_WIDTH / OVE_DISPLAY_HEIGHT */
#endif

/* stm32f7_qspi_* are arch-private (arch/arm/src/stm32f7/stm32_qspi.h — not on the app
 * include path); this is a BUILD_FLAT link so they resolve at final link.  Forward-declare. */
struct qspi_dev_s;
extern struct qspi_dev_s *stm32f7_qspi_initialize(int intf);
extern void stm32f7_qspi_enter_memorymapped(struct qspi_dev_s *dev,
					    const struct qspi_meminfo_s *meminfo, uint32_t lpto);

#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
/* Bring the N25Q128A up in memory-mapped QUAD-read mode at 0x90000000, matching the
 * proven FreeRTOS/Cube path exactly: 0xEB (QUAD I/O fast read, 1-4-4), 10 dummy
 * cycles, 108 MHz (the N25Q128A's rated quad ceiling; 216 MHz kernel clock / 2), matching
 * FreeRTOS/Cube.  The FDPIC guest XIPs its code + rodata from here, so this clock gates render
 * throughput (lvbench render 62 -> 38 ms across 54 -> 108 MHz, 5/5 clean on silicon).  Two config
 * fixes the stock NuttX path lacked — found by cross-referencing the Cube BSP field-by-field and by
 * diffing the LIVE QUADSPI registers against the working FreeRTOS build:
 *   (a) QUADSPI_CR.SSHIFT — the half-cycle sample shift the stock stm32_qspi driver forces off (its
 *       "regval |= 0x00" placeholder); restored by board patch 0002-quadspi-sample-shift.  Without
 *       it every quad read captures in the wrong window (parse fails even <=54 MHz).
 *   (b) DCR.CSHT — chip-select-high time between transactions.  Driver default is 1 cycle; Cube uses
 *       6 (CONFIG_STM32F7_QSPI_CSHT=6 in ove_board_defconfig.linux).  1 cycle (~9 ns) is too little
 *       N25Q deselect margin at 108 MHz — the SOLE DCR difference vs FreeRTOS, and why single reads
 *       corrupt worse than cached bursts (more CS toggles = more marginal transaction starts).
 * The N25Q's Volatile Config Register dummy field (bits [7:4]) MUST
 * equal the read's dummy count or quad reads return garbage; VCR is volatile so it is
 * reprogrammed every boot (read 0x85 / write-enable 0x06 / write 0x81). */
static void qspi_rootfs_memmap(void)
{
	struct qspi_dev_s *qspi = stm32f7_qspi_initialize(0);
	if (qspi == NULL)
		return;
	QSPI_SETMODE(qspi, QSPIDEV_MODE0);
	QSPI_SETBITS(qspi, 8);
	QSPI_SETFREQUENCY(qspi, 108000000); /* enter_memorymapped does not set the clock */

	struct qspi_cmdinfo_s c;
	uint8_t vcr = 0;
	c.flags = QSPICMD_READDATA;
	c.addrlen = 0;
	c.cmd = 0x85; /* READ VOLATILE CONFIG REGISTER */
	c.buflen = 1;
	c.addr = 0;
	c.buffer = &vcr;
	QSPI_COMMAND(qspi, &c);
	vcr = (uint8_t)((vcr & 0x0Fu) | (10u << 4)); /* dummy field [7:4] = 10 */
	c.flags = 0;
	c.cmd = 0x06; /* WRITE ENABLE */
	c.buflen = 0;
	c.buffer = NULL;
	QSPI_COMMAND(qspi, &c);
	c.flags = QSPICMD_WRITEDATA;
	c.cmd = 0x81; /* WRITE VOLATILE CONFIG REGISTER */
	c.buflen = 1;
	c.buffer = &vcr;
	QSPI_COMMAND(qspi, &c);

	struct qspi_meminfo_s m;
	m.flags = QSPIMEM_READ | QSPIMEM_QUADIO; /* instruction 1-line, address+data 4-line */
	m.addrlen = 3;				 /* 24-bit address (N25Q128) */
	m.dummies = 10;
	m.cmd = 0xEB; /* QUAD_INOUT_FAST_READ */
	m.buflen = 0;
	m.addr = 0;
	m.key = 0;
	m.buffer = NULL;
	stm32f7_qspi_enter_memorymapped(qspi, &m, 0 /* lpto: no auto-CS timeout */);
	/* 0x90000000..0x90FFFFFF now reads the N25Q read-only via 0xEB quad. */
}
#endif

/* FMC SDRAM read-pipe delay = 1 (SDCR1 @ 0xA0000140, RPIPE bits [14:13]).  Same fix as
 * FreeRTOS bsp_sdram_fixup: NuttX programs RPIPE=0 in stm32_enablefmc, but at the
 * 108 MHz FMC clock that leaves no read-capture margin — once the LTDC continuously
 * burst-reads the framebuffer out of this SDRAM, the CPU's uncached program-heap reads
 * go marginal and the guest heap takes bit-flips (LVGL crashes).  Nothing re-touches
 * SDCR after boot, so one OR sticks. */
static void sdram_rpipe_fix(void)
{
	volatile uint32_t *const sdcr1 = (volatile uint32_t *)0xA0000140u;
	*sdcr1 = (*sdcr1 & ~(3u << 13)) | (1u << 13); /* RPIPE = 01 */
}

#if defined(CONFIG_OVE_FB)
/* Framebuffer backend for the personality's /dev/fb0.  NuttX's own LTDC driver (fb_register at
 * board_late_initialize, CONFIG_STM32F7_LTDC) already frames a 480x272 RGB565 buffer at
 * LCD_FB_START (0xC0000000, == CONFIG_STM32F7_LTDC_FB_BASE) in SDRAM and scans it out
 * continuously, so this just hands the personality that buffer; present is a no-op (D-cache off,
 * writes reach SDRAM).  These live in board_init.c — not a separate fb_port.c — so they LINK:
 * NuttX puts the app sources in an archive, and a strong override is only pulled if the object is
 * referenced; ove_board_init references ove_hal_board_init (below), pulling this whole object, so
 * these strong ove_hal_fb_* win over the weak ove_fb.c stubs.  A standalone fb_port.c would not be
 * pulled (its only symbols are the weak-overridden ove_hal_fb_*, already resolved by ove_fb.c). */
#define FB_BASE 0xC0000000u

int ove_hal_fb_init(void)
{
	return OVE_OK; /* NuttX started LTDC scanout of FB_BASE at board_late_initialize */
}

void *ove_hal_fb_buffer(void)
{
	return (void *)FB_BASE;
}

int ove_hal_fb_get_info(struct ove_fb_info *info)
{
	info->width = OVE_DISPLAY_WIDTH;
	info->height = OVE_DISPLAY_HEIGHT;
	info->stride_bytes = OVE_DISPLAY_WIDTH * 2;
	info->fmt = OVE_FB_FMT_RGB565;
	info->smem_len = (uint32_t)OVE_DISPLAY_WIDTH * OVE_DISPLAY_HEIGHT * 2u;
	return OVE_OK;
}

void ove_hal_fb_present(void)
{
	/* No-op: the LTDC scans the SDRAM framebuffer continuously; the personality keeps the
	 * D-cache off, so /dev/fb0 writes land straight in SDRAM and appear on the next refresh. */
}
#endif /* CONFIG_OVE_FB */

/* The board's ove_hal_board_init.  The BOARD backend (nuttx_board.c's stub) is excluded from the
 * link when this file is compiled (board CMakeLists), so this is the sole definition — and
 * ove_board_init() references it, which force-pulls this object (and its ove_hal_fb_* above) into
 * the NuttX app link.  Runs from ove_app_run before ove_main(), i.e. before demo_body parses the
 * rootfs at 0x90000000. */
int ove_hal_board_init(void)
{
	sdram_rpipe_fix();
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	qspi_rootfs_memmap();
#endif
	return OVE_OK;
}
