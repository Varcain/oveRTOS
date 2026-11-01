/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */
#include "ove/ove.h"
#include <stddef.h>

void ove_test_audio_layout(size_t *values)
{
	values[0] = sizeof(struct ove_audio_node);
	values[1] = _Alignof(struct ove_audio_node);
	values[2] = offsetof(struct ove_audio_node, name_span);
	values[3] = offsetof(struct ove_audio_node, name_span.len);
	values[4] = sizeof(struct ove_audio_graph);
	values[5] = _Alignof(struct ove_audio_graph);
}

/* The POSIX backend exposes only _init filesystem handles in static mode.
 * Test-local compatibility pools exercise the Rust path facade without a
 * kernel-object allocator, mirroring embedded compatibility entry points. */
#ifndef OVE_HEAP_FS
static ove_file_storage_t files[4];
static ove_dir_storage_t directories[4];
static int file_used[4], dir_used[4];
int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	for (unsigned i = 0; i < 4; ++i) {
		int expected = 0;
		if (!__atomic_compare_exchange_n(&file_used[i], &expected, 1, 0, __ATOMIC_ACQ_REL,
						 __ATOMIC_ACQUIRE))
			continue;
		int rc = ove_fs_open_init(file, &files[i], path, flags);
		if (rc)
			__atomic_store_n(&file_used[i], 0, __ATOMIC_RELEASE);
		return rc;
	}
	return OVE_ERR_NO_MEMORY;
}
int ove_fs_close(ove_file_t file)
{
	int rc = ove_fs_close_deinit(file);
	if (!rc)
		for (unsigned i = 0; i < 4; ++i)
			if (file == &files[i])
				__atomic_store_n(&file_used[i], 0, __ATOMIC_RELEASE);
	return rc;
}
int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	for (unsigned i = 0; i < 4; ++i) {
		int expected = 0;
		if (!__atomic_compare_exchange_n(&dir_used[i], &expected, 1, 0, __ATOMIC_ACQ_REL,
						 __ATOMIC_ACQUIRE))
			continue;
		int rc = ove_fs_opendir_init(dir, &directories[i], path);
		if (rc)
			__atomic_store_n(&dir_used[i], 0, __ATOMIC_RELEASE);
		return rc;
	}
	return OVE_ERR_NO_MEMORY;
}
int ove_fs_closedir(ove_dir_t dir)
{
	int rc = ove_fs_closedir_deinit(dir);
	if (!rc)
		for (unsigned i = 0; i < 4; ++i)
			if (dir == &directories[i])
				__atomic_store_n(&dir_used[i], 0, __ATOMIC_RELEASE);
	return rc;
}
#endif
