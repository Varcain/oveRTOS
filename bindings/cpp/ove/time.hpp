/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file time.hpp
 * @brief Monotonic clock queries and delay utilities
 */

#pragma once

#include <ove/time.h>
#include <ove/types.hpp>

#ifdef CONFIG_OVE_TIME

namespace ove::time
{

/**
 * @namespace ove::time
 * @brief Thin C++ wrappers around the oveRTOS time and delay API.
 *
 * Available when `CONFIG_OVE_TIME` is enabled.
 */

/**
 * @brief Returns the current system time in microseconds.
 * @param[out] out Pointer to a `uint64_t` that receives the timestamp.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int get_us(uint64_t *out)
{
	return ove_time_get_us(out);
}

/**
 * @brief Blocks the calling task for at least the specified number of milliseconds.
 * @param[in] ms Delay duration in milliseconds.
 */
inline void delay_ms(uint32_t ms)
{
	ove_time_delay_ms(ms);
}

/**
 * @brief Blocks the calling task for at least the specified number of microseconds.
 * @param[in] us Delay duration in microseconds.
 */
inline void delay_us(uint32_t us)
{
	ove_time_delay_us(us);
}

} /* namespace ove::time */

#endif /* CONFIG_OVE_TIME */
