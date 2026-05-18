/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file board.hpp
 * @brief Board initialisation and identification functions
 */

#pragma once

#include <ove/board.h>
#include <ove/types.hpp>

#ifdef CONFIG_OVE_BOARD

namespace ove::board
{

/**
 * @namespace ove::board
 * @brief Thin C++ wrappers around the oveRTOS board description API.
 *
 * Available when `CONFIG_OVE_BOARD` is enabled.
 */

/**
 * @brief Initialises the board hardware (clocks, pin-mux, etc.).
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int init()
{
	return ove_board_init();
}

/**
 * @brief Returns the human-readable board name.
 * @return Null-terminated string with the board identifier.
 */
inline const char *name()
{
	return ove_board_name();
}

/**
 * @brief Returns a pointer to the board descriptor structure.
 * @return Pointer to a read-only `ove_board_desc` describing board capabilities.
 */
inline const struct ove_board_desc *desc()
{
	return ove_board_desc();
}

} /* namespace ove::board */

#endif /* CONFIG_OVE_BOARD */
