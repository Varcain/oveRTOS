/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_VERSION_H
#define OVE_VERSION_H

/**
 * @file ove/version.h
 * @brief oveRTOS release version.
 *
 * @defgroup ove_version Version
 * @ingroup ove_core
 * @brief The oveRTOS version, for code that must check it at compile time.
 *
 * The single source of truth for the version: the package manifests (the
 * @c ove CLI's pyproject.toml, the Rust crates' Cargo.toml and the Zig
 * build.zig.zon) carry the same number, which `make lint` checks.  It names
 * a release; the exact sources a firmware image was built from are
 * @c OVE_BUILD_OVERTOS_REV in ove/build.h.
 *
 * Plain integer macros, so preprocessor conditionals, Rust (bindgen) and
 * Zig (\@cImport) all see them.
 * @{
 */

#define OVE_VERSION_MAJOR 0 /**< Incompatible API or ABI changes. */
#define OVE_VERSION_MINOR 1 /**< Compatible additions (0..255). */
#define OVE_VERSION_PATCH 0 /**< Fixes (0..255). */

/** @brief "MAJOR.MINOR.PATCH" of the numbers above. */
#define OVE_VERSION_STRING "0.1.0"

/* Spelled out below, not through OVE_VERSION_NUMBER(): bindgen evaluates
 * object-like macros only. */
/**
 * @brief The version as one number, for comparisons: (MAJOR << 16) |
 *        (MINOR << 8) | PATCH.
 *
 * @code
 * #if OVE_VERSION >= OVE_VERSION_NUMBER(0, 2, 0)
 * ...
 * #endif
 * @endcode
 */
#define OVE_VERSION ((OVE_VERSION_MAJOR << 16) | (OVE_VERSION_MINOR << 8) | OVE_VERSION_PATCH)

/** @brief Packs a version the way @ref OVE_VERSION is packed. */
#define OVE_VERSION_NUMBER(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))

#if OVE_VERSION_MINOR > 255 || OVE_VERSION_PATCH > 255
#error "OVE_VERSION packs MINOR and PATCH into 8 bits each"
#endif

/** @} */

#endif /* OVE_VERSION_H */
