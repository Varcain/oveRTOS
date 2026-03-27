/*
 * C-compatible declarations for the TFLM-generated micro_speech model data.
 * Actual data is in the .cc files compiled as C++.
 */

#ifndef KEYWORD_MODELS_H
#define KEYWORD_MODELS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keyword classifier model (int8 quantized, ~18.8KB) */
extern const unsigned char g_micro_speech_quantized_model_data[];
#define g_micro_speech_quantized_model_data_size 18800

/* Audio preprocessor model (int8 quantized, ~8.8KB) */
extern const unsigned char g_audio_preprocessor_int8_model_data[];
#define g_audio_preprocessor_int8_model_data_size 8772

#ifdef __cplusplus
}
#endif

#endif /* KEYWORD_MODELS_H */
