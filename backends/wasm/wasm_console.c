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

static uint8_t con_buf[CONSOLE_BUF_SIZE];
static unsigned con_head;
static unsigned con_tail;
static unsigned con_count;
static pthread_mutex_t con_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t con_sem;
static int con_init_done;
static ove_console_ready_fn con_ready;
static const void *con_ready_context;

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
	ove_console_ready_fn ready = NULL;
	const void *ready_context = NULL;
	ensure_init();
	pthread_mutex_lock(&con_lock);
	if (con_count < CONSOLE_BUF_SIZE) {
		con_buf[con_head] = (uint8_t)ch;
		con_head = (con_head + 1) % CONSOLE_BUF_SIZE;
		con_count++;
		sem_post(&con_sem);
		ready = con_ready;
		ready_context = con_ready_context;
	}
	pthread_mutex_unlock(&con_lock);
	if (ready)
		ready(ready_context);
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

int ove_console_try_getchar(void)
{
	ensure_init();
	if (sem_trywait(&con_sem) != 0)
		return -1;

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

int ove_console_set_ready_callback(ove_console_ready_fn callback, const void *context)
{
	ensure_init();
	pthread_mutex_lock(&con_lock);
	con_ready_context = context;
	con_ready = callback;
	int pending = callback && con_count != 0u;
	pthread_mutex_unlock(&con_lock);
	if (pending)
		callback(context);
	return OVE_OK;
}
