/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_BOARD_H
#define OVE_HAL_BOARD_H

/**
 * @defgroup ove_hal_board HAL Board Interface
 * @brief Hardware Abstraction Layer interface for board initialisation.
 *
 * Declares the single entry-point that every board-specific HAL
 * implementation must provide.  The portable @ref ove_board layer calls
 * this function to perform platform-specific hardware setup.
 *
 * @note Board implementations supply their own definition of
 *       ove_hal_board_init() in a board-specific source file.
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Platform-specific board hardware initialisation.
 *
 * Implemented by each board HAL.  Called once by ove_board_init() to
 * configure system clocks, memory controllers, pin multiplexers, and any
 * other hardware that must be ready before higher-level subsystems start.
 *
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_board_init(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_BOARD_H */
