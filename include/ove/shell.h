/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_shell Shell
 * @ingroup ove_ui
 * @brief Interactive command-line interface over the system console.
 *
 * Implements a simple line-editing shell that tokenises input lines and
 * dispatches them to registered command handlers. Commands are registered
 * at run time via @ref ove_shell_register_cmd and driven character-by-character
 * through @ref ove_shell_process_char (typically called from a console
 * receive callback or a dedicated task).
 *
 * The maximum number of space-separated tokens per line is
 * @ref OVE_SHELL_MAX_ARGS.
 *
 * @note Requires @c CONFIG_OVE_SHELL.
 * @{
 */

#ifndef OVE_SHELL_H
#define OVE_SHELL_H

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of arguments (including command name) per shell line.
 *
 * The shell splits each input line into at most this many whitespace-delimited
 * tokens before dispatching to the command handler.
 */
#define OVE_SHELL_MAX_ARGS 8

/**
 * @brief Prototype for a shell command handler function.
 *
 * Called by the shell when the user enters a matching command. @p argc is the
 * total number of tokens (including the command name in @p argv[0]) and is
 * always at least 1. @p argv is a @c NULL-terminated array of pointers to
 * the individual tokens within the line buffer; the strings are valid only
 * for the duration of the call.
 *
 * @param[in] argc  Number of arguments (including the command name).
 * @param[in] argv  Array of @p argc argument strings, terminated by @c NULL.
 */
typedef void (*ove_shell_cmd_fn)(int argc, const char *argv[]);

/**
 * @brief Descriptor for a single shell command.
 *
 * Passed by pointer to @ref ove_shell_register_cmd. The structure must
 * remain valid (e.g. declared @c static) for the entire system lifetime
 * after registration.
 */
struct ove_shell_cmd {
	const char *name;          /**< @brief Command name used to match input tokens. */
	const char *help;          /**< @brief One-line help string shown by the built-in help command. */
	ove_shell_cmd_fn handler;  /**< @brief Function invoked when the command is matched. */
};

/**
 * @brief Shell output hook for capturing command output.
 * @param[in] data Output text.
 * @param[in] len  Length in bytes.
 */
typedef void (*ove_shell_output_hook_t)(const char *data, size_t len);

#ifdef CONFIG_OVE_SHELL

/**
 * @brief Initialise the shell subsystem.
 *
 * Sets up internal state and registers built-in commands (e.g. @c help).
 * Must be called before @ref ove_shell_register_cmd or
 * @ref ove_shell_process_char.
 *
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_SHELL.
 */
int  ove_shell_init(void);

/**
 * @brief Register a command with the shell.
 *
 * Adds the command described by @p cmd to the shell's dispatch table. The
 * @c name field is used for matching; duplicate names produce
 * implementation-defined behavior.
 *
 * @param[in] cmd  Pointer to a persistent @ref ove_shell_cmd descriptor.
 * @return OVE_OK on success, negative error code on failure (e.g. table full).
 * @note Requires @c CONFIG_OVE_SHELL.
 */
int  ove_shell_register_cmd(const struct ove_shell_cmd *cmd);

/**
 * @brief Feed one character into the shell input processor.
 *
 * Appends @p c to the internal line buffer. On receipt of a newline the
 * accumulated line is tokenised and dispatched to the matching registered
 * command handler, or an error message is printed if no match is found.
 * Backspace and other editing characters are handled transparently.
 *
 * This function is typically called from a console receive ISR, a DMA
 * callback, or a dedicated input task.
 *
 * @param[in] c  Character to process (raw byte from the console).
 * @note Requires @c CONFIG_OVE_SHELL.
 */
void ove_shell_process_char(int c);

/**
 * @brief Process a complete input line through the shell.
 *
 * Tokenises @p line and dispatches to the matching command handler.
 * Equivalent to feeding each character of the line via process_char,
 * but more convenient for programmatic use (e.g. WebSocket terminal).
 *
 * @param[in] line NUL-terminated input line (without trailing newline).
 */
void ove_shell_process_line(const char *line);

/**
 * @brief Set a hook to capture shell output.
 *
 * When set, shell command output (normally printed to console) is
 * also forwarded to this hook.  Pass NULL to remove.
 *
 * @param[in] hook Output hook function (or NULL).
 */
void ove_shell_set_output_hook(ove_shell_output_hook_t hook);

#else /* !CONFIG_OVE_SHELL */

static inline int  ove_shell_init(void) { return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_shell_register_cmd(const struct ove_shell_cmd *c) { (void)c; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_shell_process_char(int c) { (void)c; }
static inline void ove_shell_process_line(const char *l) { (void)l; }
static inline void ove_shell_set_output_hook(ove_shell_output_hook_t h) { (void)h; }

#endif /* CONFIG_OVE_SHELL */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_shell group */

#endif /* OVE_SHELL_H */
