/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file version.hpp
 * @brief oveRTOS release version, from the C header ove/version.h
 */

#pragma once

#include <ove/version.h>
#include <cstdint>
#include <string_view>

/**
 * @namespace ove::version
 * @brief The oveRTOS version as constants.
 *
 * ove/version.h is the single source of truth; the package manifests carry
 * the same number, which `make lint` checks.  This header has no package
 * metadata of its own to compare against.  The exact sources a firmware
 * image was built from are a separate matter, @c OVE_BUILD_OVERTOS_REV.
 *
 * @code
 * if constexpr (ove::version::packed >= ove::version::number(0, 2, 0)) {
 *     // something 0.2.0 added
 * }
 * @endcode
 */
namespace ove::version
{

/** @brief Major version: incompatible API or ABI changes (@c OVE_VERSION_MAJOR). */
inline constexpr std::uint32_t major = OVE_VERSION_MAJOR;
/** @brief Minor version: compatible additions, 0..255 (@c OVE_VERSION_MINOR). */
inline constexpr std::uint32_t minor = OVE_VERSION_MINOR;
/** @brief Patch version: fixes, 0..255 (@c OVE_VERSION_PATCH). */
inline constexpr std::uint32_t patch = OVE_VERSION_PATCH;
/** @brief The version as one number for comparisons, packed as by number() (@c OVE_VERSION). */
inline constexpr std::uint32_t packed = OVE_VERSION;
/** @brief "MAJOR.MINOR.PATCH" (@c OVE_VERSION_STRING). */
inline constexpr std::string_view string = OVE_VERSION_STRING;

/**
 * @brief Packs a version the way @ref packed is packed:
 *        `(major << 16) | (minor << 8) | patch` (@c OVE_VERSION_NUMBER).
 */
[[nodiscard]] constexpr std::uint32_t number(std::uint32_t maj, std::uint32_t min,
					     std::uint32_t pat) noexcept
{
	return (maj << 16) | (min << 8) | pat;
}

static_assert(packed == number(major, minor, patch), "OVE_VERSION packing");

} // namespace ove::version
