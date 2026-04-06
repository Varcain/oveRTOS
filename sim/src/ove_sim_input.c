/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_sim_input.h"

static volatile int16_t g_input_x;
static volatile int16_t g_input_y;
static volatile uint8_t g_input_pressed;

void ove_sim_input_get(int16_t *x, int16_t *y, uint8_t *pressed)
{
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
