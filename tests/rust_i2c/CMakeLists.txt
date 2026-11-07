# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.14)
project(ove_rust_i2c_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(OVE_DIR "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Use static native storage" OFF)
option(OVE_SANITIZE "Instrument the native fixture" OFF)
option(OVE_TIME "Enable the optional clock API" ON)
option(OVE_CONSOLE "Enable the optional console API" ON)
option(OVE_I2C_ENABLED "Enable I2C and its SYNC dependency" ON)
include(${OVE_DIR}/cmake/OveBindingsCommon.cmake)
set(_config "#ifndef OVE_CONFIG_H\n#define OVE_CONFIG_H\n#define CONFIG_OVE_RTOS_POSIX 1\n")
if(OVE_I2C_ENABLED)
    string(APPEND _config "#define CONFIG_OVE_I2C 1\n#define CONFIG_OVE_SYNC 1\n")
endif()
foreach(module TIME CONSOLE ZERO_HEAP)
    if(OVE_${module})
        string(APPEND _config "#define CONFIG_OVE_${module} 1\n")
    endif()
endforeach()
string(APPEND _config "#endif\n")
file(WRITE "${CMAKE_BINARY_DIR}/ove_config.h" "${_config}")
set(_sources fixture.c)
if(OVE_TIME)
    list(APPEND _sources ${OVE_DIR}/tests/host/posix/posix_time.c)
endif()
if(OVE_I2C_ENABLED)
    list(APPEND _sources ${OVE_DIR}/src/ove_sync.c ${OVE_DIR}/tests/host/posix/posix_sync.c
        ${OVE_DIR}/tests/host/posix/posix_irq.c)
    add_library(i2c_front OBJECT ${OVE_DIR}/src/ove_i2c.c)
    target_compile_definitions(i2c_front PRIVATE
        malloc=i2c_test_malloc free=i2c_test_free
        ove_i2c_init=i2c_actual_init ove_i2c_deinit=i2c_actual_deinit
        ove_i2c_destroy=i2c_actual_destroy
        ove_mutex_init=i2c_test_mutex_init ove_mutex_deinit=i2c_test_mutex_deinit
        ove_mutex_lock=i2c_test_lock ove_mutex_unlock=i2c_test_unlock)
    list(APPEND _sources $<TARGET_OBJECTS:i2c_front>)
endif()
add_library(ove_i2c_test STATIC ${_sources})
set(_targets ove_i2c_test)
if(OVE_I2C_ENABLED)
    list(APPEND _targets i2c_front)
endif()
foreach(target IN LISTS _targets)
    target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR} ${OVE_DIR}/include
        ${OVE_DIR}/backends/common ${OVE_DIR}/tests/host/posix/include)
    if(OVE_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
    endif()
endforeach()
set(OVE_I2C ${OVE_I2C_ENABLED})
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/ove_storage_sizes.c"
    "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
_ove_binding_build_sizes_probe(_sizes ove_i2c_test "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
_ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/ove_storage_sizes.env" _sizes
    "Extracting actual I2C host storage sizes")
add_custom_target(storage_sizes ALL DEPENDS "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
