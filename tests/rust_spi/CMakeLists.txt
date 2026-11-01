# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.14)
project(ove_rust_spi_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(OVE_DIR "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Use static native storage" OFF)
option(OVE_SANITIZE "Instrument the native fixture" OFF)
include(${OVE_DIR}/cmake/OveBindingsCommon.cmake)
file(READ "${OVE_DIR}/tests/ove_config.h" _config)
string(REGEX REPLACE "#define CONFIG_OVE_(LVGL|LINUX)[^\n]*\n" "" _config "${_config}")
set(_extra "#define CONFIG_OVE_SPI 1\n")
if(OVE_ZERO_HEAP)
    string(APPEND _extra "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
string(REPLACE "#endif /* OVE_CONFIG_H */" "${_extra}#endif /* OVE_CONFIG_H */" _config "${_config}")
file(WRITE "${CMAKE_BINARY_DIR}/ove_config.h" "${_config}")

add_library(spi_front OBJECT ${OVE_DIR}/src/ove_spi.c)
target_compile_definitions(spi_front PRIVATE
    malloc=spi_test_malloc free=spi_test_free
    ove_mutex_init=spi_test_mutex_init ove_mutex_deinit=spi_test_mutex_deinit
    ove_mutex_lock=spi_test_lock ove_mutex_unlock=spi_test_unlock
    ove_gpio_set=spi_test_gpio)
add_library(ove_spi_test STATIC fixture.c
    ${OVE_DIR}/tests/host/posix/posix_sync.c $<TARGET_OBJECTS:spi_front>)
foreach(_target spi_front ove_spi_test)
    target_include_directories(${_target} PRIVATE ${CMAKE_BINARY_DIR}
        ${OVE_DIR}/include ${OVE_DIR}/backends/common
        ${OVE_DIR}/tests/host/posix/include)
    if(OVE_SANITIZE)
        target_compile_options(${_target} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer)
    endif()
endforeach()
set(OVE_SPI ON)
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/ove_storage_sizes.c"
    "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
_ove_binding_build_sizes_probe(_ove_sizes ove_spi_test "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
_ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/ove_storage_sizes.env" _ove_sizes
    "Extracting actual host SPI storage sizes")
add_custom_target(storage_sizes ALL DEPENDS "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
