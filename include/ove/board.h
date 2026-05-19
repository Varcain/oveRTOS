/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_BOARD_H
#define OVE_BOARD_H

/**
 * @file board.h
 * @defgroup ove_board Board Initialization
 * @brief Board-level initialisation and identification.
 *
 * Exposes the board name, the full board descriptor, and a single
 * initialisation entry-point that sets up clocks, memory, and peripheral
 * infrastructure required before any other oveRTOS API is used.
 *
 * @note Requires @c CONFIG_OVE_BOARD.  When the option is disabled every
 *       function is replaced by a harmless stub.
 * @{
 */

#include "ove/types.h"
#include "ove/board_types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_BOARD

/**
 * @brief Initialise the board hardware.
 *
 * Configures system clocks, enables necessary peripherals, and performs
 * any board-specific low-level setup.  Must be called once before any
 * other oveRTOS API.
 *
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_board_init(void);

/**
 * @brief Return a human-readable name for the current board.
 *
 * The returned pointer is valid for the lifetime of the program.
 *
 * @return Null-terminated board name string (e.g. @c "STM32F4-Discovery").
 */
const char *ove_board_name(void);

/**
 * @brief Return a pointer to the current board's descriptor structure.
 *
 * The descriptor contains GPIO port counts, LED definitions, and MCU
 * identification fields.
 *
 * @return Pointer to a read-only @c ove_board_desc, or @c NULL if no
 *         descriptor is registered.
 */
const struct ove_board_desc *ove_board_desc(void);

#else /* !CONFIG_OVE_BOARD */

static inline int ove_board_init(void)
{
	return OVE_OK;
}
static inline const char *ove_board_name(void)
{
	return "unknown";
}
static inline const struct ove_board_desc *ove_board_desc(void)
{
	return (const struct ove_board_desc *)0;
}

#endif /* CONFIG_OVE_BOARD */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_BOARD_H */
