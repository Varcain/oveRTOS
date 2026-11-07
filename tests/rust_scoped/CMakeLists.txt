# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_rust_scoped_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(OVE_DIR "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Use claimed native storage" OFF)
option(OVE_SANITIZE "Instrument native forwarding and observers" OFF)
option(OVE_SCOPED_OPTIONAL "Enable clock and simulated IRQ critical sections" OFF)
include(${OVE_DIR}/cmake/OveBindingsCommon.cmake)
set(config "#pragma once\n#define CONFIG_OVE_RTOS_POSIX 1\n#define CONFIG_OVE_SYNC 1\n")
if(OVE_ZERO_HEAP)
    string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
if(OVE_SCOPED_OPTIONAL)
    string(APPEND config "#define CONFIG_OVE_TIME 1\n#define CONFIG_OVE_ASYNC 1\n")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/ove_config.h" "${config}")
add_library(ove_scoped_test STATIC fixture.c native.c heap.c context.c)
target_compile_options(ove_scoped_test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_include_directories(ove_scoped_test PRIVATE ${CMAKE_BINARY_DIR} ${OVE_DIR}/include
    ${OVE_DIR}/backends/common ${OVE_DIR}/tests/host/posix/include)
if(OVE_SANITIZE)
    target_compile_options(ove_scoped_test PRIVATE -fsanitize=address,undefined
        -fno-omit-frame-pointer -fno-sanitize-recover=all)
endif()
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/ove_storage_sizes.c"
    "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
_ove_binding_build_sizes_probe(_sizes ove_scoped_test "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
_ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/ove_storage_sizes.env" _sizes
    "Extracting actual native storage sizes")
add_custom_target(storage_sizes ALL DEPENDS "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
