/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Data-only relocatable ARM module for the ove_loader ELF32/ARM suite. It
 * carries R_ARM_ABS32 relocations to an import and to an internal symbol and
 * contains no code, so no Thumb-2 instruction relocations are produced.
 * Compiled with arm-none-eabi-gcc and embedded as a byte array.
 */

extern int ext_sym;	     /* import: resolved from the import table */
int *g_to_ext = &ext_sym;    /* R_ARM_ABS32 -> import */
int g_data[2] = {11, 22};    /* internal data */
int *g_to_data = &g_data[1]; /* R_ARM_ABS32 -> internal (addend = 4) */
