/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * TFLM platform port: timing hooks → ove_time_get_us.
 *
 * TensorFlow Lite Micro uses GetCurrentTimeTicks() and ticks_per_second()
 * for profiling and benchmarking.  This implementation maps them to
 * oveRTOS microsecond timestamps, which are available on all backends.
 */

#include "tensorflow/lite/micro/micro_time.h"

extern "C" {
#include "ove/time.h"
}

namespace tflite {

uint32_t GetCurrentTimeTicks() {
	uint64_t us = 0;
	ove_time_get_us(&us);
	return static_cast<uint32_t>(us);
}

uint32_t ticks_per_second() {
	return 1000000U;
}

}  // namespace tflite
