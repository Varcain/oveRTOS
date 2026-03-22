/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <_ansi.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <errno.h>
#include <reent.h>
#include <unistd.h>
#include <sys/wait.h>

#include "stm32f7xx.h"
#include "core_cm7.h"

#include "serial_wrapper.h"

/* Forward prototypes.  */
int     _isatty		(int);
int     _fstat 		(int, struct stat *);
caddr_t _sbrk		(int);
int     _close		(int);
int     _write 		(int, char *, int);
int     _lseek		(int, int, int);
int     _read		(int, char *, int);
void    _exit		(int);
int     _kill		(int, int);
int     _getpid		(void);

int _isatty (int fd)
{
	return (1);
}

int _fstat (int fd, struct stat *st)
{
	st->st_mode = S_IFCHR;
	return (0);
}

caddr_t _sbrk (int incr)
{
	extern char _heap_start;
	static char * heap_end;
	char * prev_heap_end;

	if (heap_end == NULL)
		heap_end = & _heap_start;

	prev_heap_end = heap_end;

	if ((unsigned int)heap_end + incr > __get_MSP()) {
		errno = ENOMEM;
		return ((caddr_t) -1);
	}

	heap_end += incr;

	return ((caddr_t) prev_heap_end);
}

int _close (int fd)
{
	return (0);
}

int _write (int fd, char *ptr, int len)
{
	serial_write((unsigned char *)ptr, len);
	return (len);
}

int _lseek (int fd, int ptr, int dir)
{
	return (0);
}

int _read (int fd, char * ptr, int len)
{
	return (0);
}

void _exit (int status)
{
	while (1) {}
}

int _kill (int pid, int sig)
{
	errno = EINVAL;
	return (-1);
}

int _getpid (void)
{
	return (1);
}
