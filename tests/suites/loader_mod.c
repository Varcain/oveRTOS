/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Relocatable test module for the ove_loader host suite. Compiled to a
 * freestanding ET_REL object and embedded as a byte array (see
 * tests/CMakeLists.txt + tests/cmake/embed_bin.cmake). It exercises:
 *   - an exported pure function           (mod_double)
 *   - an exported data symbol             (g_marker)
 *   - an import relocation in .data       (g_host_ref -> host_mul)
 *   - an exported function that calls the import + reads internal data
 *                                          (mod_compute)
 * It must not reference any symbol other than the imported host_mul().
 */

extern long host_mul(long a, long b); /* resolved from the import table */

long g_marker = 0x1234;		     /* exported data */
void *g_host_ref = (void *)host_mul; /* import relocation in .data */
static long g_bias = 7;		     /* internal data */

long mod_double(long x)
{
	return x * 2;
}

long mod_compute(long x)
{
	return host_mul(x, 3) + g_bias;
}

void mod_store(long *p, long v) /* writes through a caller pointer */
{
	*p = v;
}
