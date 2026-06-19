/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Module for the on-target loader-execution test. The first three functions
 * are reloc-free leaves; m_read_global exercises an R_ARM_ABS32 relocation
 * (the address of an internal global lands in a .text literal pool). Source
 * kept for regeneration of the embedded byte array (loader_mod_exec_image.h).
 */

int m_add(int a, int b)
{
	return a + b;
}

int m_mul(int a, int b)
{
	return a * b;
}

int m_neg(int x)
{
	return -x;
}

/* volatile read -> a real memory load whose address is materialised from a
 * .text literal pool patched by an R_ARM_ABS32 relocation. */
volatile int g_secret = 0x1234;

int m_read_global(void)
{
	return g_secret;
}
