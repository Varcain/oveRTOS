/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Board bindings for the FreeRTOS sampling profiler.
 *
 * ove_setup_project() (cmake/OveCommon.cmake) puts ${BOARD_DIR}/inc on
 * the include path, so any board that selects OVE_PROFILER must ship an
 * ove_board_profiler.h like this one; the backend refuses to compile
 * without the macros below. The timer must be a CMSDK APB timer, and
 * OVE_BOARD_PROFILER_HANDLER must be the name in that IRQ's vector slot,
 * or the startup code's weak default handler hangs on the first sample.
 */

#ifndef OVE_BOARD_PROFILER_H
#define OVE_BOARD_PROFILER_H

/* Sampling timer: CMSDK APB TIMER1 at 0x40001000, IRQ 9. The startup
 * code (startup_CMSDK_CM7.s) routes that IRQ to OVE_BOARD_PROFILER_HANDLER. */
#define OVE_BOARD_PROFILER_TIMER_BASE 0x40001000u
#define OVE_BOARD_PROFILER_TIMER_IRQ 9u
#define OVE_BOARD_PROFILER_HANDLER TIMER1_IRQHandler

/* RAM window scanned for the saved {r7, lr} pair of tasks whose stack
 * range is unknown (mps2_an500.ld: 4 MB at 0x20000000). */
#define OVE_BOARD_PROFILER_RAM_BASE 0x20000000u
#define OVE_BOARD_PROFILER_RAM_END 0x20400000u

/* Linker-provided .text bounds; __stext sits right after the vector
 * table so <addr|1> integer lookalikes cannot pass the is-text filter. */
#define OVE_BOARD_PROFILER_TEXT_START __stext
#define OVE_BOARD_PROFILER_TEXT_END __etext

#endif /* OVE_BOARD_PROFILER_H */
