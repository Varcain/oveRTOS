/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
typedef void (*initializer_t)(void);
extern initializer_t fixture_initializers[3];
extern unsigned int fixture_initializer_count;
#define _sinit fixture_initializers
#define _einit (fixture_initializers + fixture_initializer_count)
