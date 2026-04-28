/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Pointer input state — bridges the browser dashboard's mouse/touch
 * events into LVGL's pointer indev.
 *
 * Two distinct paths converge here:
 *
 *  1. WASM: JavaScript calls ove_sim_input_set() directly via ccall
 *     (see sim/src/ove_sim_wasm_input.c). This writes to the static
 *     `g_input_*` globals, which are then read by ove_sim_input_get()
 *     when mmap is unavailable.
 *
 *  2. POSIX host: the dashboard bridge writes click/touch events to
 *     /dev/shm/ove-input (a 16-byte struct stamped with magic
 *     "INPT"). On the first ove_sim_input_get() call we try to open
 *     and mmap that region; on success, subsequent gets read from the
 *     shm instead of the static globals. The shm is created by
 *     sim/src/ove_sim_transport_shm_local.c::local_open().
 */

#include "ove_sim_input.h"
#include "ove_config.h"

#include <stdint.h>

#if !defined(__EMSCRIPTEN__) && !defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define OVE_SIM_INPUT_HAVE_MMAP 1
#else
#define OVE_SIM_INPUT_HAVE_MMAP 0
#endif

#define INPUT_PATH "/dev/shm/ove-input"
#define INPUT_MAGIC 0x54504E49u /* "INPT" */
#define INPUT_SHM_SIZE 16

/* Layout at mmap base:
 *   offset 0: uint32_t magic
 *   offset 4: int16_t  x
 *   offset 6: int16_t  y
 *   offset 8: uint8_t  pressed
 *   offset 9: reserved
 */

static volatile int16_t g_input_x;
static volatile int16_t g_input_y;
static volatile uint8_t g_input_pressed;

#if OVE_SIM_INPUT_HAVE_MMAP
static uint8_t *g_input_shm;
static int g_input_shm_tried;
#endif

static void try_attach_shm(void)
{
#if OVE_SIM_INPUT_HAVE_MMAP
	if (g_input_shm || g_input_shm_tried)
		return;
	g_input_shm_tried = 1;

	int fd = open(INPUT_PATH, O_RDWR);
	if (fd < 0)
		return;
	void *base = mmap(NULL, INPUT_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (base == MAP_FAILED)
		return;

	uint32_t magic = 0;
	memcpy(&magic, base, sizeof(magic));
	if (magic != INPUT_MAGIC) {
		munmap(base, INPUT_SHM_SIZE);
		return;
	}
	g_input_shm = (uint8_t *)base;
#endif
}

void ove_sim_input_get(int16_t *x, int16_t *y, uint8_t *pressed)
{
	try_attach_shm();

#if OVE_SIM_INPUT_HAVE_MMAP
	if (g_input_shm) {
		int16_t sx, sy;
		uint8_t sp;
		memcpy(&sx, g_input_shm + 4, sizeof(sx));
		memcpy(&sy, g_input_shm + 6, sizeof(sy));
		memcpy(&sp, g_input_shm + 8, sizeof(sp));
		*x = sx;
		*y = sy;
		*pressed = sp;
		return;
	}
#endif

	*x = g_input_x;
	*y = g_input_y;
	*pressed = g_input_pressed;
}

void ove_sim_input_set(int16_t x, int16_t y, uint8_t pressed)
{
	g_input_x = x;
	g_input_y = y;
	g_input_pressed = pressed;
}
