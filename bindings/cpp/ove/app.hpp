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

namespace ove
{

/**
 * @brief Starts the oveRTOS scheduler and enters the main event loop.
 *
 * This function does not return under normal circumstances.  Call it at the
 * end of the C `ove_main()` entry point (or from `OVE_MAIN()`).
 */
inline void run()
{
	ove_run();
}

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
 *     static ove::audio::Graph graph;   // static local — outlives this scope
 *     setup(&graph);
 *     ove::run();
 * }
 * @endcode
 *
 * @note **Object lifetime**: any object inside `OVE_MAIN()` that a
 *       worker thread keeps a reference to must have storage that
 *       outlives this scope.  Use a `static` local (as above), a
 *       file-scope variable, or heap allocation.  Plain stack locals
 *       get popped when `OVE_MAIN()` unwinds and any captured pointer
 *       becomes dangling — the standard C++ rule for "don't return a
 *       pointer to a local", applied to workers instead of callers.
 *       On FreeRTOS the failure is immediate because the scheduler
 *       reclaims the main stack when it starts; on POSIX and others
 *       it is latent but still UB.
 */
#define OVE_MAIN()                     \
	static void ove_main_impl();   \
	extern "C" void ove_main(void) \
	{                              \
		ove_main_impl();       \
	}                              \
	static void ove_main_impl()
