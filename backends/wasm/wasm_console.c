/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM console backend.
 *
 * putchar/write go through Emscripten's stdout (Module.print).
 * getchar reads from a ring buffer fed by the dashboard via
 * an exported C function (ove_wasm_console_push).
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ── Input ring buffer ─────────────────────────────────────────────── */

#define CONSOLE_BUF_SIZE 256

static uint8_t  con_buf[CONSOLE_BUF_SIZE];
static unsigned con_head;
static unsigned con_tail;
static unsigned con_count;
static pthread_mutex_t con_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t con_sem;
static int con_init_done;

static void ensure_init(void)
{
	if (!con_init_done) {
		sem_init(&con_sem, 0, 0);
		con_init_done = 1;
	}
}

/**
 * Push a character into the console input ring.
 * Called from JS via Module.ccall('ove_wasm_console_push', ...).
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void ove_wasm_console_push(int ch)
{
	ensure_init();
	pthread_mutex_lock(&con_lock);
	if (con_count < CONSOLE_BUF_SIZE) {
		con_buf[con_head] = (uint8_t)ch;
		con_head = (con_head + 1) % CONSOLE_BUF_SIZE;
		con_count++;
		sem_post(&con_sem);
	}
	pthread_mutex_unlock(&con_lock);
}

/* ── oveRTOS console API ───────────────────────────────────────────── */

int ove_console_init(void)
{
	ensure_init();
	return OVE_OK;
}

int ove_console_getchar(void)
{
	ensure_init();
	/* Block until a character is available. */
	sem_wait(&con_sem);

	pthread_mutex_lock(&con_lock);
	int ch = -1;
	if (con_count > 0) {
		ch = con_buf[con_tail];
		con_tail = (con_tail + 1) % CONSOLE_BUF_SIZE;
		con_count--;
	}
	pthread_mutex_unlock(&con_lock);
	return ch;
}

void ove_console_putchar(int c)
{
	putchar(c);
}

void ove_console_write(const char *buf, unsigned int len)
{
	write(STDOUT_FILENO, buf, len);
}
