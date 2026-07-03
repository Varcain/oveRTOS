/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/hal/hal_board.h"
#include "ove/types.h"
#include "ove_backend_common.h"

/* Default no-op board init. A board that needs bring-up before ove_main() provides its own
 * ove_hal_board_init and EXCLUDES this BOARD backend from the NuttX link (ove_nuttx_exclude_
 * backends(BOARD)) — e.g. the STM32F746 board_init.c, which memory-maps the QSPI-XIP rootfs
 * window + applies the FMC read-pipe fix. (A weak-override here does not work: NuttX archives the
 * app sources, so a strong override in the app archive is never pulled once this resolves.) */
int ove_hal_board_init(void)
{
	return OVE_OK;
}
