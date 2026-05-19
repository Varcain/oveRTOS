/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file console.h
 * @defgroup ove_console Console
 * @ingroup ove_ui
 * @brief Low-level serial I/O for the system console.
 *
 * Provides a minimal character-oriented interface to the system console
 * (typically a UART). This API is used internally by the logging and shell
 * subsystems but may also be called directly by application code.
 *
 * The console must be initialised with @ref ove_console_init before any
 * read or write operations are performed.
 *
 * @note Requires @c CONFIG_OVE_CONSOLE.
 * @{
 */

#ifndef OVE_CONSOLE_H
#define OVE_CONSOLE_H

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_CONSOLE

/**
 * @brief Initialise the system console hardware.
 *
 * Configures the underlying serial peripheral (baud rate, framing, etc.)
 * as determined by the board support package. Must be called once before
 * @ref ove_console_getchar, @ref ove_console_putchar, or
 * @ref ove_console_write are used.
 *
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_CONSOLE.
 */
int ove_console_init(void);

/**
 * @brief Read one character from the console.
 *
 * Returns the next available character from the console receive buffer.
 * The call may block until a character is available depending on the
 * backend implementation.
 *
 * @return Character value in the range [0, 255], or -1 if no character is
 *         available or an error occurred.
 * @note Requires @c CONFIG_OVE_CONSOLE.
 */
int ove_console_getchar(void);

/**
 * @brief Write one character to the console.
 *
 * Transmits the character @p c via the console output path. The call may
 * block until the transmit buffer has space.
 *
 * @param[in] c  Character to transmit, interpreted as an @c unsigned @c char.
 * @note Requires @c CONFIG_OVE_CONSOLE.
 */
void ove_console_putchar(int c);

/**
 * @brief Write a raw byte buffer to the console.
 *
 * Transmits exactly @p len bytes from @p buf. No newline translation or
 * null termination is applied. The call may block until all bytes have been
 * accepted by the transmit buffer.
 *
 * @param[in] buf  Pointer to the data to transmit.
 * @param[in] len  Number of bytes to transmit.
 * @note Requires @c CONFIG_OVE_CONSOLE.
 */
void ove_console_write(const char *buf, unsigned int len);

#else /* !CONFIG_OVE_CONSOLE */

static inline int ove_console_init(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_console_getchar(void)
{
	return -1;
}
static inline void ove_console_putchar(int c)
{
	(void)c;
}
static inline void ove_console_write(const char *buf, unsigned int len)
{
	(void)buf;
	(void)len;
}

#endif /* CONFIG_OVE_CONSOLE */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_console group */

#endif /* OVE_CONSOLE_H */
