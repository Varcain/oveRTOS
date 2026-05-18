/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file console.hpp
 * @brief Console serial I/O functions
 */

#pragma once

#include <ove/console.h>
#include <ove/types.hpp>

#ifdef CONFIG_OVE_CONSOLE

namespace ove::console
{

/**
 * @namespace ove::console
 * @brief Thin C++ wrappers around the oveRTOS console (serial I/O) API.
 *
 * Available when `CONFIG_OVE_CONSOLE` is enabled.  All functions delegate
 * directly to the corresponding `ove_console_*` C functions.
 */

/**
 * @brief Initialises the console subsystem.
 * @return `OVE_OK` on success, or a negative error code.
 */
inline int init()
{
	return ove_console_init();
}

/* Some C libraries (NuttX, glibc) define getchar/putchar as macros;
   undefine them so the C++ inline wrappers compile cleanly. */
#ifdef getchar
#undef getchar
#endif
#ifdef putchar
#undef putchar
#endif

/**
 * @brief Reads one character from the console, blocking until one is available.
 * @return The character read as an `unsigned char` value cast to `int`, or -1
 *         on error.
 */
inline int getchar()
{
	return ove_console_getchar();
}

/**
 * @brief Writes a single character to the console output.
 * @param[in] c The character to write.
 */
inline void putchar(int c)
{
	ove_console_putchar(c);
}

/**
 * @brief Writes a buffer of bytes to the console output.
 * @param[in] data Pointer to the data to write.
 * @param[in] len  Number of bytes to write.
 */
inline void write(const char *data, unsigned int len)
{
	ove_console_write(data, len);
}

} /* namespace ove::console */

#endif /* CONFIG_OVE_CONSOLE */
