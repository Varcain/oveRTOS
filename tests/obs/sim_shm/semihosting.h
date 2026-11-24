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
#define FAKE_AUDIO_BYTES 131136u

enum { FAKE_SIM_FD, FAKE_AUDIO_FD };

extern uint8_t fake_sim[FAKE_SIM_BYTES];
extern uint8_t fake_audio[FAKE_AUDIO_BYTES];
extern uint32_t fake_pos[2];
extern bool fake_fail_next_read;

static inline int sh_open(const char *path, int mode)
{
	(void)mode;
	if (strcmp(path, "/dev/shm/ove-sim") == 0)
		return FAKE_SIM_FD;
	if (strcmp(path, "/dev/shm/ove-audio") == 0)
		return FAKE_AUDIO_FD;
	return -1;
}

static inline int sh_close(int fd)
{
	(void)fd;
	return 0;
}

static inline int sh_seek(int fd, uint32_t pos)
{
	fake_pos[fd] = pos;
	return 0;
}

static inline uint8_t *fake_file_bytes(int fd, size_t len)
{
	size_t capacity = fd == FAKE_AUDIO_FD ? FAKE_AUDIO_BYTES : FAKE_SIM_BYTES;
	uint32_t pos = fake_pos[fd];
	if (pos > capacity || len > capacity - pos)
		__builtin_trap();
	fake_pos[fd] += (uint32_t)len;
	return (fd == FAKE_AUDIO_FD ? fake_audio : fake_sim) + pos;
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
	memcpy(fake_file_bytes(fd, len), buf, len);
	return 0;
}

#endif /* SEMIHOSTING_H */
