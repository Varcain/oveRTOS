/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * ARM semihosting file I/O — raw SVC calls for bare-metal/NuttX on QEMU.
 * Works independently of the C library (no rdimon needed).
 */

#ifndef SEMIHOSTING_H
#define SEMIHOSTING_H

#include <stdint.h>
#include <stddef.h>

#define SH_SYS_OPEN   0x01
#define SH_SYS_CLOSE  0x02
#define SH_SYS_WRITE  0x05
#define SH_SYS_SEEK   0x0A

static inline uint32_t sh_call(uint32_t op, void *arg)
{
	register uint32_t r0 __asm__("r0") = op;
	register void    *r1 __asm__("r1") = arg;
	__asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
	return r0;
}

/* Open a file on the host.  mode: 0=r, 1=rb, 4=w, 5=wb, 6=r+, 7=r+b */
static inline int sh_open(const char *path, int mode)
{
	uint32_t args[3];
	args[0] = (uint32_t)(uintptr_t)path;
	args[1] = (uint32_t)mode;
	args[2] = (uint32_t)__builtin_strlen(path);
	return (int)sh_call(SH_SYS_OPEN, args);
}

static inline int sh_close(int fd)
{
	uint32_t args[1];
	args[0] = (uint32_t)fd;
	return (int)sh_call(SH_SYS_CLOSE, args);
}

/* Write len bytes. Returns 0 on full success, >0 = bytes NOT written. */
static inline int sh_write(int fd, const void *buf, size_t len)
{
	uint32_t args[3];
	args[0] = (uint32_t)fd;
	args[1] = (uint32_t)(uintptr_t)buf;
	args[2] = (uint32_t)len;
	return (int)sh_call(SH_SYS_WRITE, args);
}

/* Seek to absolute position. Returns 0 on success. */
static inline int sh_seek(int fd, uint32_t pos)
{
	uint32_t args[2];
	args[0] = (uint32_t)fd;
	args[1] = pos;
	return (int)sh_call(SH_SYS_SEEK, args);
}

#endif /* SEMIHOSTING_H */
