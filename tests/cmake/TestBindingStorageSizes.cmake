# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED OVE_ROOT OR NOT DEFINED TEST_GENERATOR OR NOT DEFINED TEST_VARIANT)
    message(FATAL_ERROR "OVE_ROOT, TEST_GENERATOR and TEST_VARIANT are required")
endif()
if(NOT TEST_VARIANT MATCHES "^(make|ninja)$")
    message(FATAL_ERROR "Unknown storage-size test variant")
endif()
if(TEST_GENERATOR STREQUAL "Ninja")
    find_program(NINJA_EXECUTABLE ninja)
    if(NOT NINJA_EXECUTABLE)
        message(STATUS "SKIP: Ninja is unavailable")
        return()
    endif()
endif()

set(WORK "${OVE_ROOT}/output/tests/binding-sizes-${TEST_VARIANT}")
set(SOURCE "${WORK}/src")
set(BUILD "${WORK}/build")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${SOURCE}/include/ove" "${SOURCE}/config/scripts")
configure_file("${OVE_ROOT}/config/scripts/extract_storage_sizes.py"
    "${SOURCE}/config/scripts/extract_storage_sizes.py" COPYONLY)
file(WRITE "${SOURCE}/include/ove/storage.h" "#include \"backend_layout.h\"\n")
file(WRITE "${SOURCE}/include/backend_layout.h"
    "struct layout { char data[8]; };\n")
file(WRITE "${SOURCE}/probe.c" [=[
#include "ove/storage.h"
unsigned char _sizeof_ove_thread_storage_t[sizeof(struct layout)];
unsigned char _alignof_ove_thread_storage_t[_Alignof(struct layout)];
]=])
set(PROJECT_TEXT [=[
cmake_minimum_required(VERSION 3.14)
project(StorageSizes C)
set(OVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("@OVE_ROOT@/cmake/OveBindingsCommon.cmake")
add_library(sizes OBJECT probe.c)
target_include_directories(sizes PRIVATE "${OVE_DIR}/include")
_ove_binding_extract_sizes("${CMAKE_CURRENT_BINARY_DIR}/sizes.env" sizes
    "Extract storage sizes")
add_custom_target(extracted ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sizes.env")
]=])
string(CONFIGURE "${PROJECT_TEXT}" PROJECT_TEXT @ONLY)
file(WRITE "${SOURCE}/CMakeLists.txt" "${PROJECT_TEXT}")

function(run_cmake)
    execute_process(COMMAND "${CMAKE_COMMAND}" ${ARGN}
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR
        TIMEOUT 40)
    if(NOT "${RESULT}" STREQUAL "0")
        message(FATAL_ERROR "cmake ${ARGN} failed: ${RESULT}\n${OUTPUT}\n${ERROR}")
    endif()
endfunction()

function(assert_sizes EXPECTED_SIZE EXPECTED_ALIGN)
    file(READ "${BUILD}/sizes.env" SIZES)
    if(NOT SIZES MATCHES "(^|\n)SIZEOF_OVE_THREAD_STORAGE=${EXPECTED_SIZE}(\n|$)" OR
       NOT SIZES MATCHES "(^|\n)ALIGNOF_OVE_THREAD_STORAGE=${EXPECTED_ALIGN}(\n|$)")
        message(FATAL_ERROR
            "Expected size ${EXPECTED_SIZE}, alignment ${EXPECTED_ALIGN}; got:\n${SIZES}")
    endif()
endfunction()

run_cmake(-G "${TEST_GENERATOR}" -S "${SOURCE}" -B "${BUILD}")
run_cmake(--build "${BUILD}")
assert_sizes(8 1)

# Change only a transitive backend header; retain the same configured build.
run_cmake(-E sleep 1)
file(WRITE "${SOURCE}/include/backend_layout.h"
    "struct layout { _Alignas(8) char data[24]; };\n")
run_cmake(--build "${BUILD}")
assert_sizes(24 8)

# A tool change must refresh the output even when its values stay identical.
file(TIMESTAMP "${BUILD}/sizes.env" BEFORE_TOOL "%s")
run_cmake(-E sleep 1)
file(APPEND "${SOURCE}/config/scripts/extract_storage_sizes.py" "\n# Refresh tool\n")
run_cmake(--build "${BUILD}")
assert_sizes(24 8)
file(TIMESTAMP "${BUILD}/sizes.env" AFTER_TOOL "%s")
if(NOT AFTER_TOOL GREATER BEFORE_TOOL)
    message(FATAL_ERROR "Changing the extraction tool did not refresh sizes.env")
endif()

run_cmake(-E sleep 1)
run_cmake(--build "${BUILD}")
file(TIMESTAMP "${BUILD}/sizes.env" AFTER_NOOP "%s")
if(NOT AFTER_NOOP STREQUAL AFTER_TOOL)
    message(FATAL_ERROR "An unchanged build rewrote sizes.env")
endif()
message(STATUS "${TEST_GENERATOR}: incremental storage sizes and alignment refresh correctly")
