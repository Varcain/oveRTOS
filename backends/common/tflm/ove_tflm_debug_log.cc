/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * TFLM platform port: DebugLog / DebugVsnprintf → ove_console_write.
 *
 * TensorFlow Lite Micro requires DebugLog() for diagnostic output and
 * DebugVsnprintf() for formatted string generation.  This implementation
 * routes them through the oveRTOS console subsystem.
 */

#include "tensorflow/lite/micro/debug_log.h"

#include <cstdarg>
#include <cstdio>

extern "C" {
#include "ove/console.h"
}

void DebugLog(const char* format, va_list args) {
	char buf[256];
	int len = vsnprintf(buf, sizeof(buf), format, args);
	if (len > 0) {
		if (len >= (int)sizeof(buf))
			len = (int)sizeof(buf) - 1;
		ove_console_write(buf, (unsigned int)len);
	}
}

int DebugVsnprintf(char* buffer, size_t buf_size, const char* format,
		   va_list vlist) {
	return vsnprintf(buffer, buf_size, format, vlist);
}
