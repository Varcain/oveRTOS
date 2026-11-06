// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g_audio_preprocessor_int8_model_data.h"
#include "g_micro_speech_quantized_model_data.h"
#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(unsigned int) == 4, "model length ABI requires four bytes");
__attribute__((aligned(16))) const unsigned char infer_tiny_model_data[23] = {
	0x31, 0x42, 0x53, 0x64, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
};
const unsigned int infer_tiny_model_data_len = sizeof(infer_tiny_model_data);

/* Independent complete-array declarations establish physical allocation truth.
 * None of these observers invokes a model program or interprets its bytes. */
const void *infer_asset_pointer(unsigned int which)
{
	switch (which) {
	case 0:
		return g_audio_preprocessor_int8_model_data;
	case 1:
		return g_micro_speech_quantized_model_data;
	case 2:
		return infer_tiny_model_data;
	default:
		return NULL;
	}
}

size_t infer_asset_length(unsigned int which)
{
	switch (which) {
	case 0:
		return sizeof(g_audio_preprocessor_int8_model_data);
	case 1:
		return sizeof(g_micro_speech_quantized_model_data);
	case 2:
		return sizeof(infer_tiny_model_data);
	default:
		return 0;
	}
}

uint8_t infer_asset_byte(unsigned int which, size_t index)
{
	if (index >= infer_asset_length(which))
		return 0;
	const unsigned char *bytes = infer_asset_pointer(which);
	return bytes[index];
}

/* Used by compiler-only strong-link controls so references cannot disappear. */
int infer_asset_consume(const void *data, size_t length)
{
	return data && length ? 0 : 1;
}
