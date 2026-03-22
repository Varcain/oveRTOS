/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file app.hpp
 * @brief Application entry point macro and scheduler start
 */

#pragma once

#include <ove/app.h>
#include <ove/types.hpp>

namespace ove {

/**
 * @brief Starts the oveRTOS scheduler and enters the main event loop.
 *
 * This function does not return under normal circumstances.  Call it at the
 * end of the C `ove_main()` entry point (or from `OVE_MAIN()`).
 */
inline void run() { ove_run(); }

} /* namespace ove */

/**
 * @def OVE_MAIN()
 * @brief Convenience macro that defines the oveRTOS application entry point.
 *
 * Declares a static `ove_main_impl()` function and bridges it to the C
 * `ove_main()` symbol expected by the oveRTOS runtime.  Usage:
 *
 * @code
 * OVE_MAIN() {
 *     // application code
 *     ove::run();
 * }
 * @endcode
 */
#define OVE_MAIN()                                \
	static void ove_main_impl();              \
	extern "C" void ove_main(void) {          \
		ove_main_impl();                  \
	}                                             \
	static void ove_main_impl()
