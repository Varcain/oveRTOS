/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Freestanding program for the on-target bFLT (uClinux flat) loader test: no
 * libc, no syscalls. flat_entry() reads a global through its absolute address
 * — a literal-pool pointer that elf2flt emits a base relocation for — so
 * executing the loaded program proves parse + base-relocation + execution on
 * the Cortex-M in one call. Built into a bFLT with elf2flt; see the
 * regeneration note in loader_flat_mod_image.h.
 */

volatile int flat_g = 0x600d;

int flat_entry(void)
{
	return flat_g + 1; /* 0x600e once flat_g's address is relocated */
}
