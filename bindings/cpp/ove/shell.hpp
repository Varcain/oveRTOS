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

#ifdef CONFIG_OVE_SHELL

namespace ove
{

/**
 * @namespace ove::shell
 * @brief Thin C++ wrappers around the oveRTOS interactive shell API.
 *
 * Available when `CONFIG_OVE_SHELL` is enabled.
 */
namespace shell
{

/**
 * @brief Initialises the shell subsystem.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int init()
{
	return ove_shell_init();
}

/**
 * @brief Registers a shell command.
 * @param[in] cmd Pointer to the command descriptor structure.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int register_cmd(const struct ove_shell_cmd *cmd)
{
	return ove_shell_register_cmd(cmd);
}

/**
 * @brief Feeds a single character into the shell input processor.
 *
 * Typically called from the console receive path.
 *
 * @param[in] c The character received from the console.
 */
inline void process_char(int c)
{
	ove_shell_process_char(c);
}

/**
 * @brief Process a complete input line through the shell.
 * @param[in] line NUL-terminated command line.
 */
inline void process_line(const char *line)
{
	ove_shell_process_line(line);
}

/**
 * @brief Set a hook to capture shell output.
 * @param[in] hook Output hook function (nullptr to remove).
 */
inline void set_output_hook(ove_shell_output_hook_t hook)
{
	ove_shell_set_output_hook(hook);
}

} /* namespace shell */

} // namespace ove

#endif /* CONFIG_OVE_SHELL */
