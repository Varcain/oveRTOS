/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file shell.hpp
 * @brief Interactive shell command registration
 */

#pragma once

#include <ove/shell.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_SHELL

namespace ove::shell
{

/**
 * @namespace ove::shell
 * @brief Thin C++ wrappers around the oveRTOS interactive shell API.
 *
 * Available when `CONFIG_OVE_SHELL` is enabled.
 */

/**
 * @brief Initialises the shell subsystem.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> init() noexcept
{
	return from_rc(ove_shell_init());
}

/**
 * @brief Registers a shell command.
 *
 * The descriptor is copied, but the @c name and @c help strings are not:
 * they must stay valid while the command is registered. Registering a name
 * again replaces that command, even when the table is full.
 *
 * @param[in] cmd Pointer to the command descriptor structure.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> register_cmd(const struct ove_shell_cmd *cmd) noexcept
{
	return from_rc(ove_shell_register_cmd(cmd));
}

/**
 * @brief Feeds a single character into the shell input processor.
 *
 * Call it from thread context, typically the console input task, never
 * from an ISR: the matching command's handler runs inside this call. Feed
 * characters from one thread, as the line being typed is shared;
 * process_line has its own copy and may run on any thread.
 *
 * @param[in] c The character received from the console.
 */
inline void process_char(int c)
{
	ove_shell_process_char(c);
}

/**
 * @brief Process a complete input line through the shell.
 *
 * Thread context only, from any thread or a command handler; it does not
 * touch the line process_char is building. At most 127 characters are
 * used.
 *
 * @param[in] line NUL-terminated command line.
 */
inline void process_line(const char *line)
{
	ove_shell_process_line(line);
}

/**
 * @brief Set a hook to capture shell output.
 *
 * The hook runs on the command's thread with the shell's lock held, so it
 * must not wait for another thread that is using the shell.
 *
 * @param[in] hook Output hook function (nullptr to remove).
 */
inline void set_output_hook(ove_shell_output_hook_t hook)
{
	ove_shell_set_output_hook(hook);
}

} /* namespace ove::shell */

#endif /* CONFIG_OVE_SHELL */
