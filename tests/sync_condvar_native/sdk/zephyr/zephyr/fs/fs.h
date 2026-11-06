/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_CONDVAR_MODEL_FS_H
#define OVE_CONDVAR_MODEL_FS_H
#include <stdint.h>
struct fs_file_t {
	uintptr_t opaque;
};
struct fs_dir_t {
	uintptr_t opaque;
};
#endif
