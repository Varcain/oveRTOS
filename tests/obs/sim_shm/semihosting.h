/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* In-memory shared-memory files the test inspects and edits as the host would. */

#ifndef SEMIHOSTING_H
#define SEMIHOSTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FAKE_SIM_BYTES (64u + 2u * 65536u)
#define FAKE_FB_BYTES (1u << 20)
#define FAKE_AUDIO_BYTES 131136u

enum { FAKE_SIM_FD, FAKE_FB_FD, FAKE_AUDIO_FD, FAKE_FILE_COUNT };

extern uint8_t fake_sim[FAKE_SIM_BYTES];
extern uint8_t fake_fb[FAKE_FB_BYTES];
extern uint8_t fake_audio[FAKE_AUDIO_BYTES];
extern bool fake_fail_next_read;

static struct fake_file {
	const char *path;
	uint8_t *bytes;
	size_t capacity;
	uint32_t pos;
	bool exists;
	bool writable;
} fake_files[FAKE_FILE_COUNT] = {
	[FAKE_SIM_FD] = {.path = "/dev/shm/ove-sim",
			 .bytes = fake_sim,
			 .capacity = sizeof(fake_sim)},
	[FAKE_FB_FD] = {.path = "/dev/shm/ove-fb", .bytes = fake_fb, .capacity = sizeof(fake_fb)},
	[FAKE_AUDIO_FD] = {.path = "/dev/shm/ove-audio",
			   .bytes = fake_audio,
			   .capacity = sizeof(fake_audio)},
};

static inline int sh_open(const char *path, int mode)
{
	for (int fd = 0; fd < FAKE_FILE_COUNT; fd++) {
		struct fake_file *file = &fake_files[fd];
		if (strcmp(path, file->path) != 0)
			continue;
		if (!file->exists && mode < 4)
			return -1;
		/* Discard contents on creation or truncation. */
		if (!file->exists || (mode >= 4 && mode < 8))
			memset(file->bytes, 0, file->capacity);
		file->exists = true;
		file->writable = mode >= 2;
		file->pos = 0;
		return fd;
	}
	return -1;
}

static inline int sh_close(int fd)
{
	(void)fd;
	return 0;
}

static inline int sh_seek(int fd, uint32_t pos)
{
	fake_files[fd].pos = pos;
	return 0;
}

static inline uint8_t *fake_file_bytes(int fd, size_t len)
{
	struct fake_file *file = &fake_files[fd];
	uint32_t pos = file->pos;
	if (pos > file->capacity || len > file->capacity - pos)
		__builtin_trap();
	file->pos += (uint32_t)len;
	return file->bytes + pos;
}

static inline int sh_read(int fd, void *buf, size_t len)
{
	if (fake_fail_next_read) {
		fake_fail_next_read = false;
		return (int)len;
	}
	memcpy(buf, fake_file_bytes(fd, len), len);
	return 0;
}

static inline int sh_write(int fd, const void *buf, size_t len)
{
	if (!fake_files[fd].writable)
		return (int)len;
	memcpy(fake_file_bytes(fd, len), buf, len);
	return 0;
}

#endif /* SEMIHOSTING_H */
