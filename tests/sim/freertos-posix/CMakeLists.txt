# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.20)
project(ove_test_cpp_freertos_posix C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(FREERTOS_BACKEND "${OVE_ROOT}/backends/freertos")
set(FREERTOS_PORT "${FREERTOS_PATH}/portable/ThirdParty/GCC/Posix")
if(NOT EXISTS "${FREERTOS_PORT}/port.c")
    message(FATAL_ERROR
        "The C++ and Rust tests require FREERTOS_PATH to name the manifest-pinned kernel.")
endif()
set(CMOCKA "${OVE_ROOT}/dl/cmocka")
if(NOT EXISTS "${CMOCKA}/src/cmocka.c")
    message(FATAL_ERROR
        "CMocka source is missing. Run make test-cpp-freertos-posix "
        "or make test-rust-freertos-posix to fetch it.")
endif()

add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter
                    $<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>)

set(KERNEL_SOURCES
    ${FREERTOS_PATH}/tasks.c ${FREERTOS_PATH}/queue.c ${FREERTOS_PATH}/list.c
    ${FREERTOS_PATH}/timers.c
    ${FREERTOS_PORT}/port.c ${FREERTOS_PORT}/utils/wait_for_event.c)
option(OVE_ZERO_HEAP "Build the zero-heap test variant" OFF)
if(OVE_ZERO_HEAP)
    add_compile_definitions(CONFIG_OVE_ZERO_HEAP=1)
    set(HEAP_STUBS ${FREERTOS_BACKEND}/freertos_heap_stubs.c)
else()
    list(APPEND KERNEL_SOURCES ${FREERTOS_PATH}/portable/MemMang/heap_4.c)
endif()
set_source_files_properties(${KERNEL_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wno-error;-Wno-missing-field-initializers;-Wno-cast-function-type;-Wno-sign-compare")

add_library(cmocka-static STATIC ${CMOCKA}/src/cmocka.c)
target_include_directories(cmocka-static PUBLIC ${CMOCKA}/include PRIVATE ${CMOCKA}/src)
target_compile_definitions(cmocka-static PRIVATE
    HAVE_STRINGS_H HAVE_INTTYPES_H HAVE_MALLOC_H HAVE_SIGNAL_H
    HAVE_SETJMP_H HAVE_STDARG_H HAVE_STDDEF_H HAVE_STDINT_H
    HAVE_STDIO_H HAVE_STDLIB_H HAVE_STRING_H
    HAVE_CALLOC HAVE_EXIT HAVE_FREE HAVE_LONGJMP HAVE_MALLOC
    HAVE_MEMCPY HAVE_MEMSET HAVE_SETJMP HAVE_STRCMP HAVE_SPRINTF
    HAVE_VSNPRINTF HAVE_FPRINTF HAVE_PRINTF)
target_compile_options(cmocka-static PRIVATE -Wno-error)

add_library(ove_freertos_posix STATIC posix_glue.c rust_runner.c
    ${KERNEL_SOURCES} ${HEAP_STUBS} ${FREERTOS_BACKEND}/freertos_thread.c
    ${FREERTOS_BACKEND}/freertos_sync.c ${OVE_ROOT}/src/ove_sync.c)
target_include_directories(ove_freertos_posix
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../freertos-qemu
    ${FREERTOS_PATH}/include ${FREERTOS_PORT} ${FREERTOS_PORT}/utils
    ${OVE_ROOT}/include ${FREERTOS_BACKEND}/include ${OVE_ROOT}/backends/common
    INTERFACE
    ${OVE_ROOT}/bindings/cpp ${OVE_ROOT}/tests)

add_executable(ove_test_cpp_freertos_posix cpp_main.cpp ${OVE_ROOT}/tests/cpp/suites/test_mutex.cpp)
target_link_libraries(ove_test_cpp_freertos_posix PRIVATE ove_freertos_posix cmocka-static pthread)

if(NOT OVE_ZERO_HEAP)
    add_executable(ove_test_freertos_libc_malloc libc_malloc_test.c
        ${FREERTOS_PATH}/portable/MemMang/heap_4.c
        ${FREERTOS_BACKEND}/freertos_libc_malloc.c)
    target_include_directories(ove_test_freertos_libc_malloc PRIVATE
        $<TARGET_PROPERTY:ove_freertos_posix,INCLUDE_DIRECTORIES>)
    target_compile_options(ove_test_freertos_libc_malloc PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(ove_test_freertos_libc_malloc PRIVATE -fsanitize=address -fno-omit-frame-pointer)
endif()

set(OVE_DIR "${OVE_ROOT}")
include(${OVE_DIR}/cmake/OveBindingsCommon.cmake)
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/ove_storage_sizes.c"
    "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
_ove_binding_build_sizes_probe(_sizes ove_freertos_posix "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
_ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/ove_storage_sizes.env" _sizes
    "Extracting FreeRTOS POSIX storage sizes")
add_custom_target(storage_sizes ALL DEPENDS "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
