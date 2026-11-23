/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/** @file trace.hpp
 * @brief User trace marks and zones. Marks and zones are no-ops without markers.
 */

#pragma once

#include <ove/trace.h>

#include <cstdint>

namespace ove::trace
{

/**
 * @brief Append a name from a single registering thread.
 *
 * Copies up to 31 bytes into one of 32 entries. Re-registering an id appends
 * an entry, and the latest entry wins. Returns false for a null or empty name,
 * a full table, or disabled markers.
 */
[[nodiscard]] inline bool name(std::uint16_t id, const char *name) noexcept
{
	return ove_trace_register_name(id, name);
}

/** @brief Emit a point mark on the current thread or interrupt lane. */
inline void mark(std::uint16_t id) noexcept
{
	ove_trace_mark(id);
}

/** @brief Open a zone on the current lane. Zones with the same id may nest. */
inline void zone_begin(std::uint16_t id) noexcept
{
	ove_trace_zone_begin(id);
}

/** @brief Close the innermost open zone with this id on the current lane. */
inline void zone_end(std::uint16_t id) noexcept
{
	ove_trace_zone_end(id);
}

/** @brief Open a zone on construction and close it on destruction.
 * Destroy the guard on the lane that opened it. Moving transfers ownership.
 */
class Zone
{
      public:
	explicit Zone(std::uint16_t id) noexcept : id_(id), active_(true)
	{
		zone_begin(id_);
	}

	~Zone() noexcept
	{
		if (active_)
			zone_end(id_);
	}

	Zone(Zone &&other) noexcept : id_(other.id_), active_(other.active_)
	{
		other.active_ = false;
	}

	Zone &operator=(Zone &&) = delete;
	Zone(const Zone &) = delete;
	Zone &operator=(const Zone &) = delete;

      private:
	std::uint16_t id_;
	bool active_;
};

} // namespace ove::trace
