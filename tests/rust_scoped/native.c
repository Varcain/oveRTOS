/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define ove_mutex_init native_mutex_init
#define ove_mutex_deinit native_mutex_deinit
#define ove_mutex_lock native_mutex_lock
#define ove_mutex_unlock native_mutex_unlock
#define ove_recursive_mutex_init native_recursive_mutex_init
#define ove_recursive_mutex_deinit native_recursive_mutex_deinit
#define ove_recursive_mutex_lock native_recursive_mutex_lock
#define ove_recursive_mutex_unlock native_recursive_mutex_unlock
#define ove_condvar_init native_condvar_init
#define ove_condvar_deinit native_condvar_deinit
#define ove_condvar_wait native_condvar_wait
#define ove_condvar_wait_checked native_condvar_wait_checked
#define ove_condvar_signal native_condvar_signal
#define ove_condvar_broadcast native_condvar_broadcast
#include "../host/posix/posix_sync.c"
