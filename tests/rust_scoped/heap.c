/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define ove_mutex_create native_mutex_create
#define ove_mutex_destroy native_mutex_destroy
#define ove_recursive_mutex_create native_recursive_mutex_create
#define ove_recursive_mutex_destroy native_recursive_mutex_destroy
#define ove_condvar_create native_condvar_create
#define ove_condvar_destroy native_condvar_destroy
#include "../../src/ove_sync.c"
