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
 * The dashboard side writes click/touch events to /dev/shm/ove-input
 * (a 16-byte struct stamped with magic "INPT"), and the QEMU guest reads
 * it through semihosting on every get. The dashboard side creates the
 * file before the firmware starts (qemu-run.sh, or qemu-wasm.js in the
 * browser).
 *
 * With no file (a headless run) the pointer reads as released at the
 * origin.
 */

#include "ove_sim_input.h"
#include "semihosting.h"

#include <stdint.h>
#include <string.h>

#define INPUT_PATH "/dev/shm/ove-input"
#define INPUT_MAGIC 0x54504E49u /* "INPT" */
#define INPUT_SHM_SIZE 16

/* Layout of the file:
 *   offset 0: uint32_t magic
 *   offset 4: int16_t  x
 *   offset 6: int16_t  y
 *   offset 8: uint8_t  pressed
 *   offset 9: reserved
 */

static int g_input_fd = -1;
static int g_input_fd_tried;

/* Read the pointer file through semihosting.  Returns 0 when there is no
 * usable file, which the first failed open makes permanent. */
static int semihosting_get(int16_t *x, int16_t *y, uint8_t *pressed)
{
	uint8_t raw[INPUT_SHM_SIZE];
	uint32_t magic;

	if (g_input_fd < 0) {
		if (g_input_fd_tried)
			return 0;
		g_input_fd_tried = 1;
		g_input_fd = sh_open(INPUT_PATH, 1); /* rb */
		if (g_input_fd < 0)
			return 0;
	}

	sh_seek(g_input_fd, 0);
	if (sh_read(g_input_fd, raw, sizeof(raw)) != 0)
		return 0;
	memcpy(&magic, raw, sizeof(magic));
	if (magic != INPUT_MAGIC)
		return 0;

	memcpy(x, raw + 4, sizeof(*x));
	memcpy(y, raw + 6, sizeof(*y));
	*pressed = raw[8];
	return 1;
}

void ove_sim_input_get(int16_t *x, int16_t *y, uint8_t *pressed)
{
	if (semihosting_get(x, y, pressed))
		return;

	*x = 0;
	*y = 0;
	*pressed = 0;
}
