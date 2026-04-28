/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * POSIX syscall stubs for picolibc tinystdio on bare-metal STM32F7.
 *
 * Picolibc tinystdio's stdout/stderr backend calls write() (not _write),
 * but picolibc.specs links libgloss-style aliases via the toolchain so
 * the underscore-prefixed names also resolve.  We implement the canonical
 * underscore form; picolibc's libc internally aliases write() → _write().
 *
 * Newlib-only headers (_ansi.h, reent.h) and the _isatty/_fstat callbacks
 * are no longer pulled — picolibc tinystdio doesn't query isatty()/fstat()
 * the way newlib's full stdio does.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

#include "stm32f7xx.h"
#include "core_cm7.h"

#include "serial_wrapper.h"

/* Forward prototypes.  */
caddr_t _sbrk(int);
int _close(int);
int _write(int, char *, int);
int _lseek(int, int, int);
int _read(int, char *, int);
void _exit(int);
int _kill(int, int);
int _getpid(void);

caddr_t _sbrk(int incr)
{
	extern char _heap_start;
	static char *heap_end;
	char *prev_heap_end;

	if (heap_end == NULL)
		heap_end = &_heap_start;

	prev_heap_end = heap_end;

	if ((unsigned int)heap_end + incr > __get_MSP()) {
		errno = ENOMEM;
		return ((caddr_t)-1);
	}

	heap_end += incr;

	return ((caddr_t)prev_heap_end);
}

int _close(int fd)
{
	return (0);
}

int _write(int fd, char *ptr, int len)
{
	serial_write((unsigned char *)ptr, len);
	return (len);
}

int _lseek(int fd, int ptr, int dir)
{
	return (0);
}

int _read(int fd, char *ptr, int len)
{
	return (0);
}

void _exit(int status)
{
	while (1) {
	}
}

int _kill(int pid, int sig)
{
	errno = EINVAL;
	return (-1);
}

int _getpid(void)
{
	return (1);
}

/*
 * Picolibc tinystdio FILE streams.
 *
 * Picolibc's <stdio.h> declares stdin/stdout/stderr as `FILE *const`,
 * leaving the application to provide the storage. printf/scanf bodies
 * inside libc.a take the *address* of these pointers, so when the
 * application's code path actually pulls libc printf in (e.g. lwip /
 * mbedtls debug paths in example_net), missing stream definitions
 * trigger an `undefined reference to 'stdout'` link error.
 *
 * Define a single shared FILE that calls _write() for output and a no-op
 * read; tinystdio's vfprintf reaches the serial UART through that. The
 * three pointers all alias the same FILE — boards without a real input
 * device don't distinguish stdin, and stderr unbuffered semantics are
 * irrelevant on bare metal where each _write goes straight to the wire.
 */
static int _stdio_putc(char c, FILE *stream)
{
	(void)stream;
	serial_write((unsigned char *)&c, 1);
	return (unsigned char)c;
}

static int _stdio_getc(FILE *stream)
{
	(void)stream;
	return _FDEV_EOF;
}

static FILE _stdio_file = FDEV_SETUP_STREAM(_stdio_putc, _stdio_getc, NULL, _FDEV_SETUP_RW);

FILE *const stdin = &_stdio_file;
FILE *const stdout = &_stdio_file;
FILE *const stderr = &_stdio_file;
