/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * TFLM platform port: arena allocation helpers.
 *
 * Provides macros for allocating the tensor arena in both heap and
 * zero-heap modes.  The arena is a contiguous byte buffer that TFLM
 * uses for all intermediate tensor storage during inference.
 */

#ifndef OVE_TFLM_PORT_H
#define OVE_TFLM_PORT_H

#include "ove_config.h"

/*
 * Recommended alignment for the tensor arena.  CMSIS-NN kernels on
 * Cortex-M may require 16-byte aligned loads for SIMD operations.
 */
#define OVE_TFLM_ARENA_ALIGN 16

/*
 * Declare a statically allocated tensor arena with correct alignment.
 *
 *   OVE_TFLM_ARENA_DEFINE(my_arena, 32768);
 *   // my_arena is uint8_t[32768] with 16-byte alignment.
 */
#define OVE_TFLM_ARENA_DEFINE(name, size) \
	static uint8_t __attribute__((aligned(OVE_TFLM_ARENA_ALIGN))) name[(size)]

#endif /* OVE_TFLM_PORT_H */
