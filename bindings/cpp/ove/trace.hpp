/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file trace.hpp
 * @brief Named user trace markers and zones
 */

#pragma once

#include <ove/trace.h>

#include <cstdint>

namespace ove::trace
{

/**
 * @namespace ove::trace
 * @brief Thin C++ wrappers around the user instrumentation API.
 *
 * All calls are no-ops when the trace stream or marker support is
 * compiled out; they remain safe to leave in hot paths. Marks and zones
 * may also be emitted from an interrupt handler; their records then land
 * on the interrupt lane.
 */

/**
 * @brief Register a name for a user trace id.
 *
 * The trace layer copies the name, cut to 31 bytes, so it does not have
 * to outlive the call: a temporary @c std::string::c_str() is safe to
 * pass. Register names from one thread only: two threads registering at
 * once can lose a name.
 *
 * @param[in] id   User id (0..65535).
 * @param[in] name Name of the user trace id.
 * @return @c true when the name was registered, @c false when it was
 *         rejected (empty name, or the name table is full).
 */
[[nodiscard]] inline bool name(std::uint16_t id, const char *name) noexcept
{
	return ove_trace_register_name(id, name);
}

/**
 * @brief Emit a point mark on the calling thread's lane.
 * @param[in] id User id (0..65535).
 */
inline void mark(std::uint16_t id) noexcept
{
	ove_trace_mark(id);
}

/**
 * @brief Open a named zone.
 * @param[in] id User id (0..65535).
 */
inline void zone_begin(std::uint16_t id) noexcept
{
	ove_trace_zone_begin(id);
}

/**
 * @brief Close the most recent zone with @p id on this thread: the
 *        innermost one still open.
 * @param[in] id User id (0..65535).
 */
inline void zone_end(std::uint16_t id) noexcept
{
	ove_trace_zone_end(id);
}

/**
 * @brief RAII zone: opens on construction, closes on destruction.
 *
 * Move is allowed; the moved-from object no longer closes the zone. The
 * end closes the zone on the calling thread, so destroy a zone on the
 * thread that opened it.
 */
class Zone
{
      public:
	explicit Zone(std::uint16_t id) noexcept : id_(id), active_(true)
	{
		ove_trace_zone_begin(id_);
	}

	~Zone() noexcept
	{
		if (active_)
			ove_trace_zone_end(id_);
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
