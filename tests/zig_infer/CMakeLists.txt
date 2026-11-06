# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_zig_infer_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_DIR "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Use caller-owned inference storage" OFF)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(${OVE_DIR}/cmake/OveBindingsCommon.cmake)
file(READ "${OVE_DIR}/tests/ove_config.h" config)
string(REGEX REPLACE "#define CONFIG_OVE_(LVGL|LINUX)[^\n]*\n" "" config "${config}")
set(extra "#define CONFIG_OVE_INFER 1\n")
if(OVE_ZERO_HEAP)
 string(APPEND extra "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
string(REPLACE "#endif /* OVE_CONFIG_H */" "${extra}#endif /* OVE_CONFIG_H */" config "${config}")
file(WRITE "${CMAKE_BINARY_DIR}/ove_config.h" "${config}")
add_library(ove_zig_infer_fixture STATIC fixture.c)
target_compile_options(ove_zig_infer_fixture PRIVATE -Wall -Wextra -Werror)
target_include_directories(ove_zig_infer_fixture PRIVATE "${CMAKE_BINARY_DIR}"
 "${OVE_DIR}/include" "${OVE_DIR}/tests/host/posix/include")
set(OVE_INFER ON)
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/ove_storage_sizes.c"
 "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
_ove_binding_build_sizes_probe(sizes ove_zig_infer_fixture "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
_ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/ove_storage_sizes.env" sizes
 "Extracting actual host inference storage sizes")
add_custom_target(storage_sizes ALL DEPENDS "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
set(model_dir "${CMAKE_BINARY_DIR}/models")
set(model_files
 "${model_dir}/g_audio_preprocessor_int8_model_data.c"
 "${model_dir}/g_audio_preprocessor_int8_model_data.h"
 "${model_dir}/g_micro_speech_quantized_model_data.c"
 "${model_dir}/g_micro_speech_quantized_model_data.h")
add_custom_command(OUTPUT ${model_files}
 COMMAND ${Python3_EXECUTABLE} "${OVE_DIR}/models/convert.py"
  --model-dir "${OVE_DIR}/models/keyword" --output-dir "${model_dir}"
 DEPENDS "${OVE_DIR}/models/convert.py"
  "${OVE_DIR}/models/keyword/audio_preprocessor_int8.tflite"
  "${OVE_DIR}/models/keyword/micro_speech_quantized.tflite"
 VERBATIM)
add_library(ove_zig_infer_assets STATIC assets.c ${model_files})
target_include_directories(ove_zig_infer_assets PRIVATE "${model_dir}")
target_compile_options(ove_zig_infer_assets PRIVATE -Wall -Wextra -Werror)
