/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* In-memory stand-in for the an500 board's semihosting.h: the plugin
 * shared-memory file is a byte array the test inspects and edits as the
 * host would. */

#ifndef SEMIHOSTING_H
#define SEMIHOSTING_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FAKE_SIM_BYTES (64u + 2u * 65536u)

extern uint8_t fake_sim[FAKE_SIM_BYTES];
extern uint32_t fake_pos;

static inline int sh_open(const char *path, int mode)
{
	(void)mode;
	return strcmp(path, "/dev/shm/ove-sim") == 0 ? 3 : -1;
}

static inline int sh_close(int fd)
{
	(void)fd;
	return 0;
}

static inline int sh_seek(int fd, uint32_t pos)
{
	(void)fd;
	fake_pos = pos;
	return 0;
}

static inline int sh_read(int fd, void *buf, size_t len)
{
	(void)fd;
	if (fake_pos + len > FAKE_SIM_BYTES)
		__builtin_trap();
	memcpy(buf, fake_sim + fake_pos, len);
	fake_pos += (uint32_t)len;
	return 0;
}

static inline int sh_write(int fd, const void *buf, size_t len)
{
	(void)fd;
	if (fake_pos + len > FAKE_SIM_BYTES)
		__builtin_trap();
	memcpy(fake_sim + fake_pos, buf, len);
	fake_pos += (uint32_t)len;
	return 0;
}

#endif /* SEMIHOSTING_H */
