/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Module for the on-target loader-execution test:
 *   - m_add/m_mul/m_neg: reloc-free leaves
 *   - m_read_global:     R_ARM_ABS32 (global address via a .text literal pool)
 *   - m_use_import:      R_ARM_THM_CALL (BL to a far firmware import, which
 *                        exceeds the +/-16 MB Thumb-BL reach -> exercises a
 *                        loader-generated veneer)
 * Source kept for regeneration of the embedded byte array
 * (loader_mod_exec_image.h).
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

extern int host_add(int a, int b); /* far firmware import */

/* "+ 1" keeps this from being a tail call, so the import is reached via a
 * BL (R_ARM_THM_CALL) rather than a B.W tail branch. */
int m_use_import(int x)
{
	return host_add(x, 100) + 1;
}
