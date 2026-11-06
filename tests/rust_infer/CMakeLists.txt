# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_rust_infer_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(OVE_DIR "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Use caller-owned inference storage" OFF)
option(OVE_SANITIZE "Instrument the initialized native fixture" OFF)
include(${OVE_DIR}/cmake/OveBindingsCommon.cmake)
file(READ "${OVE_DIR}/tests/ove_config.h" _config)
string(REGEX REPLACE "#define CONFIG_OVE_(LVGL|LINUX)[^\n]*\n" "" _config "${_config}")
set(_extra "#define CONFIG_OVE_INFER 1\n")
if(OVE_ZERO_HEAP)
 string(APPEND _extra "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
string(REPLACE "#endif /* OVE_CONFIG_H */" "${_extra}#endif /* OVE_CONFIG_H */" _config "${_config}")
file(WRITE "${CMAKE_BINARY_DIR}/ove_config.h" "${_config}")
add_library(ove_infer_test STATIC fixture.c)
target_compile_options(ove_infer_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(ove_infer_test PRIVATE ${CMAKE_BINARY_DIR}
 ${OVE_DIR}/include ${OVE_DIR}/tests/host/posix/include)
if(OVE_SANITIZE)
 target_compile_options(ove_infer_test PRIVATE -fsanitize=address,undefined
  -fno-omit-frame-pointer)
endif()
set(OVE_INFER ON)
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/ove_storage_sizes.c"
 "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
_ove_binding_build_sizes_probe(_ove_sizes ove_infer_test "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
_ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/ove_storage_sizes.env" _ove_sizes
 "Extracting actual host inference storage sizes")
add_custom_target(storage_sizes ALL DEPENDS "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
