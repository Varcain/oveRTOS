/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Board bindings for the Zephyr sampling profiler.
 *
 * boards/qemu-mps2-an500/zephyr/CMakeLists.txt puts this directory on the
 * include path (${BOARD_DIR}/inc), so any board that selects OVE_PROFILER
 * may ship an ove_board_profiler.h like this one. When it is present the
 * profiler takes its samples in the timer's own interrupt, which runs
 * above the board's driver interrupts - see
 * backends/zephyr/zephyr_profiler.c. Boards without it fall back to the
 * k_timer path, where the samples are taken in the system-clock interrupt.
 */

#ifndef OVE_BOARD_PROFILER_H
#define OVE_BOARD_PROFILER_H

/* Sampling timer: CMSDK APB TIMER1 at 0x40001000, NVIC line 9 - the same
 * timer the FreeRTOS backend uses (see
 * boards/qemu-mps2-an500/freertos/inc/ove_board_profiler.h). The timer is
 * clocked by the system clock (25 MHz on the MPS2-AN500) and is not used
 * by the kernel (Zephyr drives its clock from the Cortex-M SysTick). */
#define OVE_BOARD_PROFILER_TIMER_BASE 0x40001000u
#define OVE_BOARD_PROFILER_TIMER_IRQ 9u

#endif /* OVE_BOARD_PROFILER_H */
