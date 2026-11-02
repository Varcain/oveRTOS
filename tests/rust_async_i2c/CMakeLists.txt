# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.14)
project(ove_rust_async_i2c_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(OVE_DIR "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Use static binding storage" OFF)
option(OVE_I2C_ENABLED "Enable native I2C" ON)
option(OVE_ASYNC_ENABLED "Enable native async" ON)
option(OVE_SANITIZE "Instrument the native fixture" OFF)
include(${OVE_DIR}/cmake/OveBindingsCommon.cmake)
set(_config "#ifndef OVE_CONFIG_H\n#define OVE_CONFIG_H\n#define CONFIG_OVE_RTOS_POSIX 1\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_TIMER 1\n#define CONFIG_OVE_TIME 1\n")
if(OVE_I2C_ENABLED)
 string(APPEND _config "#define CONFIG_OVE_I2C 1\n")
endif()
if(OVE_ASYNC_ENABLED)
 string(APPEND _config "#define CONFIG_OVE_ASYNC 1\n")
endif()
if(OVE_ZERO_HEAP)
 string(APPEND _config "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
string(APPEND _config "#endif\n")
file(WRITE "${CMAKE_BINARY_DIR}/ove_config.h" "${_config}")
add_library(async_i2c_front OBJECT ${OVE_DIR}/src/ove_i2c.c)
target_compile_definitions(async_i2c_front PRIVATE
    malloc=async_i2c_test_malloc free=async_i2c_test_free
    ove_mutex_init=async_i2c_test_mutex_init ove_mutex_deinit=async_i2c_test_mutex_deinit)
if(OVE_I2C_ENABLED AND OVE_ASYNC_ENABLED)
    set(_fixture fixture.c)
endif()
add_library(ove_async_i2c_test STATIC ${_fixture}
    ${OVE_DIR}/tests/host/posix/posix_sync.c ${OVE_DIR}/tests/host/posix/posix_irq.c
    $<TARGET_OBJECTS:async_i2c_front>)
foreach(_target async_i2c_front ove_async_i2c_test)
    target_include_directories(${_target} PRIVATE ${CMAKE_BINARY_DIR}
        ${OVE_DIR}/include ${OVE_DIR}/backends/common
        ${OVE_DIR}/tests/host/posix/include)
    if(OVE_SANITIZE)
        target_compile_options(${_target} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer -g)
    endif()
endforeach()
set(OVE_I2C ${OVE_I2C_ENABLED})
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/ove_storage_sizes.c"
    "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
_ove_binding_build_sizes_probe(_ove_sizes ove_async_i2c_test "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
_ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/ove_storage_sizes.env" _ove_sizes
    "Extracting actual async I2C storage sizes")
add_custom_target(storage_sizes ALL DEPENDS "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
