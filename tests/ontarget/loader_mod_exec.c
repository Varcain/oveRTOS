/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Reloc-free leaf functions for the on-target loader-execution test. With no
 * globals and no calls the compiled .text carries no relocations, so loading
 * is simply "copy .text, enter". Source kept for regeneration of the embedded
 * byte array (see loader_mod_exec_image.h).
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
