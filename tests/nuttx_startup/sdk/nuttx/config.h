/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#define CONFIG_BUILD_FLAT 1
#define CONFIG_HAVE_CXXINITIALIZE 1
#if defined(FIXTURE_NATIVE_UNIT) && PRESERVED_DEFINITION != 17
#error "The startup helper must preserve existing source definitions"
#endif
#if !defined(FIXTURE_NATIVE_UNIT) && defined(lib_cxx_initialize)
#error "Only the original native initializer may be renamed"
#endif
