/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file log.h
 * @defgroup ove_log Logging
 * @ingroup ove_ui
 * @brief Compile-time filtered logging macros over the system console.
 *
 * Provides four severity levels: error, warning, info, and debug. The
 * active level is controlled at compile time by defining @c OVE_LOG_LEVEL
 * to one of the @c OVE_LOG_LEVEL_* constants before including this header;
 * messages at levels above the configured threshold expand to no-ops and
 * generate no code.
 *
 * Each macro formats a message with @c snprintf into a 256-byte stack buffer
 * and writes it through @ref ove_console_write. A trailing newline is
 * appended automatically.
 *
 * @note Logging output requires @c CONFIG_OVE_CONSOLE.
 * @{
 */

#ifndef OVE_LOG_H
#define OVE_LOG_H

#include "ove/console.h"
#include "ove_config.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Log level constants
 * Numeric severity levels in ascending verbosity order. Pass one of these
 * as the value of @c OVE_LOG_LEVEL to control the compile-time filter.
 * @{
 */
/** @brief Error level — non-recoverable failures. */
#define OVE_LOG_LEVEL_ERR 0
/** @brief Warning level — unexpected but recoverable conditions. */
#define OVE_LOG_LEVEL_WRN 1
/** @brief Info level — normal operational milestones (default). */
#define OVE_LOG_LEVEL_INF 2
/** @brief Debug level — verbose diagnostic output. */
#define OVE_LOG_LEVEL_DBG 3
/** @} */

/**
 * @brief Compile-time log level threshold.
 *
 * All log macros with a severity numerically greater than this value expand
 * to no-ops. Defaults to @ref OVE_LOG_LEVEL_INF if not defined externally.
 */
#ifndef OVE_LOG_LEVEL
#define OVE_LOG_LEVEL OVE_LOG_LEVEL_INF
#endif

/** @cond INTERNAL */
#ifdef CONFIG_OVE_NET_HTTPD
void ove_httpd_log_append(const char *line);
#define _OVE_LOG_HTTPD_HOOK(buf) ove_httpd_log_append(buf)
#else
#define _OVE_LOG_HTTPD_HOOK(buf) ((void)0)
#endif

/* Sim log broadcast is now handled by sim_console.c's ove_console_write,
 * so no separate hook is needed in the log macro. */
#define _OVE_LOG_SIM_HOOK(buf, len) ((void)0)

#define _OVE_LOG_OUTPUT(prefix, fmt, ...)                                                        \
	do {                                                                                     \
		char _ove_log_buf[256];                                                          \
		int _ove_log_len =                                                               \
			snprintf(_ove_log_buf, sizeof(_ove_log_buf), prefix fmt, ##__VA_ARGS__); \
		if (_ove_log_len > 0) {                                                          \
			_ove_log_buf[_ove_log_len] = '\n';                                       \
			_ove_log_buf[_ove_log_len + 1] = '\0';                                   \
			ove_console_write(_ove_log_buf, (unsigned int)(_ove_log_len + 1));       \
			_ove_log_buf[_ove_log_len] = '\0';                                       \
			_OVE_LOG_HTTPD_HOOK(_ove_log_buf);                                       \
			_OVE_LOG_SIM_HOOK(_ove_log_buf, (unsigned int)_ove_log_len);             \
		}                                                                                \
	} while (0)
/** @endcond */

/**
 * @brief Log an error message.
 *
 * Emits a @c "[E] " prefixed, newline-terminated message via the console.
 * Expands to a no-op when @c OVE_LOG_LEVEL < @ref OVE_LOG_LEVEL_ERR.
 *
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 */
#if OVE_LOG_LEVEL >= OVE_LOG_LEVEL_ERR
#define OVE_LOG_ERR(fmt, ...) _OVE_LOG_OUTPUT("[E] ", fmt, ##__VA_ARGS__)
#else
#define OVE_LOG_ERR(fmt, ...) \
	do {                  \
	} while (0)
#endif

/**
 * @brief Log a warning message.
 *
 * Emits a @c "[W] " prefixed, newline-terminated message via the console.
 * Expands to a no-op when @c OVE_LOG_LEVEL < @ref OVE_LOG_LEVEL_WRN.
 *
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 */
#if OVE_LOG_LEVEL >= OVE_LOG_LEVEL_WRN
#define OVE_LOG_WRN(fmt, ...) _OVE_LOG_OUTPUT("[W] ", fmt, ##__VA_ARGS__)
#else
#define OVE_LOG_WRN(fmt, ...) \
	do {                  \
	} while (0)
#endif

/**
 * @brief Log an informational message.
 *
 * Emits a @c "[I] " prefixed, newline-terminated message via the console.
 * Expands to a no-op when @c OVE_LOG_LEVEL < @ref OVE_LOG_LEVEL_INF.
 *
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 */
#if OVE_LOG_LEVEL >= OVE_LOG_LEVEL_INF
#define OVE_LOG_INF(fmt, ...) _OVE_LOG_OUTPUT("[I] ", fmt, ##__VA_ARGS__)
#else
#define OVE_LOG_INF(fmt, ...) \
	do {                  \
	} while (0)
#endif

/**
 * @brief Log a debug message.
 *
 * Emits a @c "[D] " prefixed, newline-terminated message via the console.
 * Expands to a no-op when @c OVE_LOG_LEVEL < @ref OVE_LOG_LEVEL_DBG.
 *
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 */
#if OVE_LOG_LEVEL >= OVE_LOG_LEVEL_DBG
#define OVE_LOG_DBG(fmt, ...) _OVE_LOG_OUTPUT("[D] ", fmt, ##__VA_ARGS__)
#else
#define OVE_LOG_DBG(fmt, ...) \
	do {                  \
	} while (0)
#endif

/**
 * @brief Internal helper: format and emit a raw (unprefixed) console line.
 *
 * Used by framework internals where the caller controls newlines and
 * prefixes. Output is truncated to 256 bytes.
 *
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 */
#define _OVE_LOG_RAW(fmt, ...)                                                            \
	do {                                                                              \
		char _ove_log_buf[256];                                                   \
		int _ove_log_len =                                                        \
			snprintf(_ove_log_buf, sizeof(_ove_log_buf), fmt, ##__VA_ARGS__); \
		if (_ove_log_len > 0) {                                                   \
			ove_console_write(_ove_log_buf, (unsigned int)_ove_log_len);      \
		}                                                                         \
	} while (0)

/**
 * @brief Emit a raw (unprefixed) log message.
 *
 * Writes a formatted string directly to the console without adding any
 * severity prefix or automatic newline. The caller is responsible for
 * including @c "\\n" in the format string if needed.
 *
 * @note This macro exists for backward compatibility. Prefer the typed
 *       variants (@ref OVE_LOG_ERR, @ref OVE_LOG_WRN, @ref OVE_LOG_INF,
 *       @ref OVE_LOG_DBG) for new code.
 *
 * @param[in] fmt  printf-style format string.
 * @param[in] ...  Format arguments.
 */
#define OVE_LOG(fmt, ...) _OVE_LOG_RAW(fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_log group */

#endif /* OVE_LOG_H */
